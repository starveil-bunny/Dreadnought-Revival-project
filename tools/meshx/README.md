# meshx

Exports ship meshes (skeletal meshes, their material instances and textures)
from the client's paks to glTF, for the admin dashboard's 3D ship preview.
Driven by `scripts/gen-admin-art.py --models`; see Program.cs for the
interface. Uses CUE4Parse + CUE4Parse-Conversion (NuGet); SkiaSharp's Linux
native assets are pinned to 2.88.x, the range the bundled SkiaSharp accepts.
