fv_module(KIND qt
          DEPS fv_base fv_walkthrough fv_archive
          THIRD_PARTY pugixml::pugixml)

# python-docx's default template parts (data/README.md).
fv_embed(data/word_styles.xml data/word_styles_with_effects.xml data/word_numbering.xml
         data/word_thumbnail.jpeg)
