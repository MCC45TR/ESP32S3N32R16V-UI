import codecs
import shutil
from pathlib import Path

web_dir = Path(__file__).resolve().parent
paths = [
    web_dir / 'cad_model.h',
    web_dir / 'svg_images.h',
]

for p in paths:
    with p.open('rb') as f:
        data = f.read()
    
    shutil.copy(p, p.with_suffix(p.suffix + '.bak'))
    
    if data.startswith(codecs.BOM_UTF16_LE):
        text = data.decode('utf-16le')
        with p.open('w', encoding='utf-8') as f:
            f.write(text)
        print(f'Converted {p} from UTF-16LE to UTF-8 without BOM')
    elif data.startswith(codecs.BOM_UTF16_BE):
        text = data.decode('utf-16be')
        with p.open('w', encoding='utf-8') as f:
            f.write(text)
        print(f'Converted {p} from UTF-16BE to UTF-8 without BOM')
    else:
        # Check if there are null bytes which might indicate utf-16 without bom
        if b'\x00' in data[:20]:
            try:
                text = data.decode('utf-16')
                with p.open('w', encoding='utf-8') as f:
                    f.write(text)
                print(f'Converted {p} from UTF-16 (no BOM) to UTF-8 without BOM')
            except UnicodeDecodeError:
                print(f'Could not convert {p}')
        else:
            print(f'{p} is already not UTF-16 or handled. Saving as UTF-8 just in case.')
            try:
                text = data.decode('utf-8')
                with p.open('w', encoding='utf-8') as f:
                    f.write(text)
            except Exception as e:
                print('Error:', e)
