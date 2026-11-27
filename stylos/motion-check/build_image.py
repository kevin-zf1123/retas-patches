def prepare(images):
    # The foreground hooks still use the original motion-linked host image.
    # The standalone renderer is a managed file, independent of the host variant.
    for variant in ("chs", "chs-laa", "precanvas"):
        images.variant(images.linked(variant, "base").read_bytes(),
                       images.linked(variant, "main").read_bytes())
