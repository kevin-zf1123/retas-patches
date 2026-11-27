#include "retas/vector_texture.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace retas {
namespace {
using numeric_detail::binary64;
using Matrix=std::array<double,9>;
constexpr std::uint64_t storage_limit=512ull*1024*1024;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector texture preparation cancelled");}
Matrix multiply(const Matrix& a,const Matrix& b){
    Matrix c{};
    for(std::size_t r=0;r<3;++r){const auto i=r*3;
        c[i]=binary64(binary64(binary64(a[i]*b[0])+binary64(a[i+2]*b[6]))+binary64(a[i+1]*b[3]));
        c[i+1]=binary64(binary64(binary64(a[i+2]*b[7])+binary64(a[i]*b[1]))+binary64(a[i+1]*b[4]));
        c[i+2]=binary64(binary64(binary64(a[i+1]*b[5])+binary64(a[i+2]*b[8]))+binary64(a[i]*b[2]));
    }
    return c;
}
Matrix matrix(const VectorTextureMaterial& m){
    const float sx=std::bit_cast<float>(m.scale_x_bits),sy=std::bit_cast<float>(m.scale_y_bits),
        rotation=std::bit_cast<float>(m.rotation_bits),shear=std::bit_cast<float>(m.shear_x_bits);
    if(!std::isfinite(sx)||!std::isfinite(sy)||sx==0||sy==0||!std::isfinite(rotation)||!std::isfinite(shear)||
       m.origin_x==std::numeric_limits<int>::min()||m.origin_y==std::numeric_limits<int>::min())
        throw std::invalid_argument("Unsupported vector texture transform");
    Matrix v{1,0,-double(m.origin_x),0,1,-double(m.origin_y),0,0,1};
    // RCWCMN10598C60 native degree conversion; x87 trig is represented by
    // host stdlib trig, with its original-runtime evidence boundary explicit.
    const auto theta=binary64(-double(rotation)*0x1.1df46a2529d39p-6);
    // Native FSIN/FCOS leave ST0 unchanged with C2 when |theta|>=2^63.
    // Host trig would silently normalize a fundamentally different matrix.
    // Retain raw angles but reject this branch before image preparation.
    if(std::abs(theta)>=0x1p63)throw std::invalid_argument("Vector texture rotation exceeds native trig input domain");
    const auto s=binary64(std::sin(theta)),c=binary64(std::cos(theta));
    v=multiply(Matrix{c,-s,0,s,c,0,0,0,1},v);
    v=multiply(Matrix{1,-double(shear),0,0,1,0,0,0,1},v);
    const auto x=binary64(1.0/double(sx)),y=binary64(1.0/double(sy));
    for(std::size_t i=0;i<3;++i){v[i]=binary64(v[i]*x);v[i+3]=binary64(v[i+3]*y);}
    for(auto value:v)if(!std::isfinite(value))throw std::invalid_argument("Nonfinite vector texture inverse matrix");
    return v;
}
std::uint16_t word(const Bytes& b,std::size_t i){return std::uint16_t(unsigned(b[i])|(unsigned(b[i+1])<<8));}
}
bool VectorTextureMaterial::operator==(const VectorTextureMaterial& b) const{
    if(flags!=b.flags||origin_x!=b.origin_x||origin_y!=b.origin_y||scale_x_bits!=b.scale_x_bits||scale_y_bits!=b.scale_y_bits||rotation_bits!=b.rotation_bits||shear_x_bits!=b.shear_x_bits||
       image.width!=b.image.width||image.height!=b.image.height||image.depth!=b.image.depth||image.color_order!=b.image.color_order||
       std::bit_cast<std::uint64_t>(image.resolution)!=std::bit_cast<std::uint64_t>(b.image.resolution)||image.tiles.size()!=b.image.tiles.size())return false;
    auto x=image.tiles.begin(),y=b.image.tiles.begin();for(;x!=image.tiles.end();++x,++y)
        if(x->first!=y->first||bool(x->second)!=bool(y->second)||(x->second&&*x->second!=*y->second))return false;
    return true;
}
std::uint64_t vector_texture_storage(const VectorTextureMaterial& m){
    (void)matrix(m);const auto& image=m.image;
    if((m.flags&15)!=1&&(m.flags&15)!=2)throw std::invalid_argument("Unknown vector texture cursor mode");
    if(image.width<=0||image.height<=0||image.width>100000||image.height>100000||std::int64_t(image.width)*image.height>268435456||
       !std::isfinite(image.resolution)||image.resolution<=0||(image.depth!=1&&image.depth!=32&&image.depth!=64)||image.color_order!=(image.depth==1?0:3))
        throw std::invalid_argument("Unsupported embedded vector texture image");
    const auto columns=(image.width+255)/256,rows=(image.height+255)/256;
    if(std::size_t(columns)*std::size_t(rows)!=image.tiles.size())throw std::invalid_argument("Vector texture requires every image tile");
    const auto stride=image.tile_bytes();std::uint64_t bytes=sizeof(VectorTextureMaterial);
    for(const auto& [pos,tile]:image.tiles){
        if(pos.x<0||pos.y<0||pos.x>=columns||pos.y>=rows||!tile||tile->size()!=stride)
            throw std::invalid_argument("Invalid embedded vector texture tile");
        const auto n=std::uint64_t(stride)+128;
        if(n>storage_limit-bytes)throw std::length_error("Vector texture image storage exceeds512MiB");bytes+=n;
    }
    return bytes;
}
std::array<double,6> vector_texture_inverse_matrix(const VectorTextureMaterial& m){
    const auto v=matrix(m);std::array<double,6> out{};std::copy_n(v.begin(),6,out.begin());return out;
}
void admit_vector_texture_rectangle(const VectorTextureMaterial& m,std::int32_t l,std::int32_t t,std::int32_t r,std::int32_t b){
    admit_vector_texture_rectangle(vector_texture_inverse_matrix(m),l,t,r,b);
}
void admit_vector_texture_rectangle(const std::array<double,6>& v,std::int32_t l,std::int32_t t,std::int32_t r,std::int32_t b){
    if(r<l||b<t)throw std::invalid_argument("Invalid vector texture raw query rectangle");
    for(auto x:{l,r})for(auto y:{t,b})for(std::size_t i:{0u,3u}){
        const auto q=binary64(binary64(binary64(double(x)*v[i])+binary64(double(y)*v[i+1]))+v[i+2]);
        if(!std::isfinite(q)||std::trunc(q)<double(std::numeric_limits<int>::min())||std::trunc(q)>double(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Vector texture cursor conversion outside signed domain");
    }
}
VectorTextureSampler::VectorTextureSampler(const VectorTextureMaterial& m,std::stop_token stop):VectorTextureSampler(m,vector_texture_inverse_matrix(m),stop){}
VectorTextureSampler::VectorTextureSampler(const VectorTextureMaterial& m,const std::array<double,6>& inverse,std::stop_token stop){
    for(double value:inverse)if(!std::isfinite(value))throw std::invalid_argument("Nonfinite effective texture inverse matrix");
    poll(stop);const auto storage=vector_texture_storage(m);
    if(storage>storage_limit/2)throw std::length_error("Vector texture source and snapshot exceed512MiB");
    matrix_=inverse;
    material_.flags=m.flags;material_.origin_x=m.origin_x;material_.origin_y=m.origin_y;material_.scale_x_bits=m.scale_x_bits;material_.scale_y_bits=m.scale_y_bits;material_.rotation_bits=m.rotation_bits;material_.shear_x_bits=m.shear_x_bits;
    material_.image=Raster(m.image.width,m.image.height,m.image.depth,m.image.resolution);
    for(const auto& [pos,tile]:m.image.tiles){poll(stop);material_.image.tiles.emplace(pos,std::make_shared<Bytes>(*tile));}
    poll(stop);
}
std::array<double,2> VectorTextureSampler::transform(std::int32_t x,std::int32_t y) const{
    return {binary64(binary64(binary64(double(x)*matrix_[0])+binary64(double(y)*matrix_[1]))+matrix_[2]),
        binary64(binary64(binary64(double(x)*matrix_[3])+binary64(double(y)*matrix_[4]))+matrix_[5])};
}
void VectorTextureSampler::admit_rectangle(std::int32_t l,std::int32_t t,std::int32_t r,std::int32_t b) const{
    admit_vector_texture_rectangle(matrix_,l,t,r,b);
}
Pixel VectorTextureSampler::sample(std::int32_t raw_x,std::int32_t raw_y) const{
    const auto v=transform(raw_x,raw_y);std::array<int,2> point{};const auto& image=material_.image;
    for(std::size_t i=0;i<2;++i){if(!std::isfinite(v[i])||std::trunc(v[i])<double(std::numeric_limits<int>::min())||std::trunc(v[i])>double(std::numeric_limits<int>::max()))throw std::invalid_argument("Vector texture cursor conversion outside signed domain");
        const int extent=i?image.height:image.width;int coordinate=int(std::trunc(v[i]));
        if((material_.flags&15)==1){coordinate%=extent;if(coordinate<0)coordinate+=extent;}else coordinate=std::clamp(coordinate,0,extent-1);point[i]=coordinate;}
    const auto& tile=*image.tiles.at({point[0]/256,point[1]/256});const auto p=std::size_t(point[1]%256)*256+std::size_t(point[0]%256);
    if(image.depth==1)return tile[p/8]&(0x80u>>(p%8))?Pixel{0,0,0,65535}:Pixel{0,0,0,0};
    if(image.depth==32)return Pixel::rgba8(tile[p*4+2],tile[p*4+1],tile[p*4],tile[p*4+3]);
    return {word(tile,p*8+4),word(tile,p*8+2),word(tile,p*8),word(tile,p*8+6)};
}
}
