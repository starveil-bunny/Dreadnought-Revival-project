// meshx: export ship meshes from the client's paks to glTF for the admin
// dashboard's 3D preview (scripts/gen-admin-art.py --models).
//
//   meshx <Paks dir> <out dir> < object paths, one per line
//     ("/Game/Generic/Ships/Assault/Heavy/VH_AssaultH_SK.VH_AssaultH_Sk")
//
// Writes <out>/<asset>.glb (LOD0, skin kept, no textures inside) and for each
// material its instance parameters (<inst>.json) and textures (.png), all
// flat in <out>. CUE4Parse names the files with Windows separators on Linux
// ("\tmp\out\DreadGame\...\X.glb" in the working directory), so the export
// runs in <out> and each such file is renamed to its last path segment.
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Versions;
using CUE4Parse_Conversion;
using CUE4Parse_Conversion.Options;

var provider = new DefaultFileProvider(args[0], SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE4_13));
provider.Initialize();
provider.Mount();
var outDir = Path.GetFullPath(args[1]);
Directory.CreateDirectory(outDir);
Environment.CurrentDirectory = outDir;
var session = new ExportSession(null);
string? line;
int queued = 0;
while ((line = Console.ReadLine()) != null)
{
    line = line.Trim();
    if (line == "") continue;
    var ap = line.StartsWith("/Game/") ? "DreadGame/Content/" + line[6..] : line;
    try { session.Add(provider.LoadPackageObject(ap)); queued++; }
    catch (Exception e) { Console.WriteLine($"miss {line}: {e.Message}"); }
}
Console.WriteLine($"queued {queued}");
int ok = 0, bad = 0;
await session.RunAsync(outDir, new ExportOptions(meshFormat: EMeshFormat.Gltf2, meshQuality: EMeshQuality.Highest,
        exportMaterials: true, textureFormat: ETextureFormat.Png),
    new Progress<ExportProgress>(p => { if (p.LastResult.Success) ok++; else { bad++; Console.WriteLine($"fail {p.LastResult.ObjectPath}: {p.LastResult.Error?.Message}"); } }),
    CancellationToken.None);
foreach (var f in Directory.GetFiles(outDir))
{
    var name = Path.GetFileName(f);
    int i = name.LastIndexOf('\\');
    if (i >= 0) File.Move(f, Path.Combine(outDir, name[(i + 1)..]), true);
}
Console.WriteLine($"exported {ok}, failed {bad}");
