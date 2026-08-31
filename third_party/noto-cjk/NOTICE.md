# Noto Sans CJK KR font atlas

Sura OS uses raster data generated from `NotoSansCJKkr-Regular.otf` for its
antialiased ASCII and modern Korean UI font.

- Upstream: https://github.com/notofonts/noto-cjk
- Source file: https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/Korean/NotoSansCJKkr-Regular.otf
- Pinned SHA-256: `6BCB2A0703AA137E874FC2DFFA85F6C21BA9A67FA329E81B8C801663AF7E992A`
- Generator: `tools/sura_ui_font_generate.ps1`
- Generated atlas: `stdlib/freestanding/font_ui_atlas.sura`
- License: [SIL Open Font License 1.1](OFL.txt)

The original OTF is downloaded into the ignored build cache during regeneration;
it is not stored in this repository. The generated raster atlas remains Font
Software covered by the SIL Open Font License 1.1.
