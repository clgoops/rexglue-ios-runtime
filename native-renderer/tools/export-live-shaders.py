"""Export the actual native presentation/UI MSL for Apple compiler checks."""
from pathlib import Path
import re,sys
root=Path(__file__).resolve().parents[1]
output=Path(sys.argv[1]);output.mkdir(parents=True,exist_ok=True)
names=[]
for name in ('rex_command_processor.cpp','rex_immediate.cpp','upload_probe.cpp','metal_backend.mm'):
    source=(root/'src'/name).read_text()
    for symbol,msl in re.findall(r'constexpr const char\* (k\w+)=R"\((.*?)\)";',source,re.S):
        (output/(symbol+'.metal')).write_text(msl+'\n')
        names.append(symbol)
assert set(names)=={'kSwapVertex','kSwapFragment','kUIVertex','kUIFragment','kProbeVertex','kProbeFragment','kProbeIndexVertex','kProbeIndexFragment','kUploadVertex'},names
print('Exported nine production swap/UI/upload qualification shader entry points')
