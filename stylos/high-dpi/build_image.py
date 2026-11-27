def prepare(images):
    images.variant(images.linked("chs-laa","base").read_bytes(),images.linked("precanvas","base").read_bytes())
