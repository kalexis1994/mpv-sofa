# MPV Sofa — imágenes para Steam

Los archivos de `export/` están listos para cargar en los campos homónimos de Steamworks. Se compusieron con el logo original de `assets/icons/mpv-sofa-logo.png`, la tipografía Dosis incluida en el proyecto y dos artes generados usando ese logo como referencia. El nombre formal visible en las piezas es **MPV Sofa**.

| Campo de Steamworks | Archivo | Tamaño |
|---|---|---:|
| Store Header Capsule | `export/store_header.png` | 920 × 430 |
| Store Small Capsule | `export/store_small.png` | 462 × 174 |
| Store Main Capsule | `export/store_main.png` | 1232 × 706 |
| Store Vertical Capsule | `export/store_vertical.png` | 748 × 896 |
| Library Capsule | `export/library_capsule.png` | 600 × 900 |
| Library Header Capsule | `export/library_header.png` | 920 × 430 |
| Library Hero | `export/library_hero.png` | 3840 × 1240 |
| Library Logo (transparente) | `export/library_logo.png` | 1280 × 360 |
| Shortcut Icon | `export/shortcut_icon.png` | 256 × 256 |
| App Icon | `export/app_icon.jpg` | 184 × 184 |
| Page Background (opcional) | `export/page_background.png` | 1438 × 810 |
| Screenshots reales | `export/screenshots/01-home.png` a `05-cinema-grain.png` | 1920 × 1080 |

Las capturas de pantalla son del software funcionando, sin texto de marketing ni interfaces inventadas. La captura de reproducción se descartó porque el controlador OpenGL virtual usado para esta sesión dejó el vídeo negro. Antes de publicar, conviene reemplazar una de las capturas de ajustes por una captura real de reproducción hecha en una máquina con salida gráfica normal. Las capturas se tomaron antes de actualizar el título visible del programa a `MPV Sofa`; por eso la primera aún muestra el wordmark anterior.

Steam exige al menos cinco capturas 16:9 a 1920 × 1080 o más, y recomienda marcar como aptas para todas las edades al menos cuatro de ellas si corresponde. Las cápsulas no contienen reclamos, premios, precios ni texto adicional. El hero de biblioteca no lleva palabras; el logo de biblioteca tiene transparencia. Los recursos para eventos y bundles no se incluyen porque dependen de una publicación o bundle concretos.

Para regenerar las cápsulas y los iconos, ejecutar `build_assets.py` con Python y Pillow. No modifica las capturas.

Referencias oficiales:

- https://partner.steamgames.com/doc/store/assets
- https://partner.steamgames.com/doc/store/assets/standard
- https://partner.steamgames.com/doc/store/assets/libraryassets
- https://partner.steamgames.com/doc/store/assets/rules
