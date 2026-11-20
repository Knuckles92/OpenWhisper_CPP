# python-docx's default template parts

`word_styles.xml`, `word_styles_with_effects.xml`, `word_numbering.xml` and
`word_thumbnail.jpeg` are `word/styles.xml`, `word/stylesWithEffects.xml`,
`word/numbering.xml` and `docProps/thumbnail.jpeg` of python-docx 1.2.0's
`docx/templates/default.docx`, unchanged. OpenWhisper writes its Word files from that
template, so `documents/ooxml.cpp` writes every package with them (`fv_embed` in
`CMakeLists.txt`).

python-docx is MIT-licensed: Copyright (c) 2013 Steve Canny, https://github.com/scanny.
