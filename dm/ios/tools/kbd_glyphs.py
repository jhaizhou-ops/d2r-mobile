# Mac App 的键盘键帽：玩家用键盘时，按键提示条 / 技能格 / 页签两端画的手柄键图标换成对应的键盘键名（宿主 dm_padmode.c 把取图路径改到这一套）。
# 22 张原版键帽（Controller/ControllerButtons/switch/，48×45）里有键盘对应键的那些各出一张同尺寸的：底板取原版肩键那块石板的配色（左缘一列 + 板面一列，
# 竖向拉到键盘键的高），键名用原版界面字体白字黑边；没有键盘对应键的（右摇杆、十字键、刷新小图标）原图不动。键位同 host/front_mac.m key_bit。
# Keyboard key caps of the Mac App: when the player uses the keyboard, the gamepad button icons drawn on the key-hint bar / skill slots / ends of tab rows are replaced by the corresponding keyboard key names (the host's dm_padmode.c switches the image path to this set).
# Of the 22 original key caps (Controller/ControllerButtons/switch/, 48x45), each one with a keyboard counterpart gets an image of the same size: the plate takes the colours of the stone slab of the original shoulder button (one column from the left edge + one column of the face,
# stretched vertically to the height of a keyboard key), and the key name is in the original UI font, white with a black outline; those without a keyboard counterpart (right stick, D-pad, the small refresh icon) keep the original image. Key mapping as in host/front_mac.m key_bit.
import json, os, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw, ImageFont
import romfs_art, gen_widestash_art as GW

DIR = 'data/hd/global/ui/Controller/ControllerButtons/switch/'
KEYS = {'ButtonRight': 'J', 'ButtonBottom': 'K', 'ButtonTop': 'U', 'ButtonLeft': 'I', 'BumperLeft': 'Q', 'BumperLeftRectangle': 'Q', 'BumperRight': 'E', 'BumperRightRectangle': 'E',
        'TriggerLeft': 'Z', 'TriggerRight': 'C', 'ButtonStart': '=', 'ButtonSelect': 'Tab', 'StickLeftButton': 'F', 'StickRightButton': 'G', 'StickLeft': 'WASD'}
ALL = 22
S = 4   # 先按 4 倍画再缩回去：小图上的圆角与字边才平滑 | draw at 4x and scale back down: rounded corners and glyph edges are only smooth that way on a small image

def _cap(plate, label, font):
    """plate = 原版肩键图（取配色）；返回 48×45 的键盘键帽。 | plate = the original shoulder-button image (for its colours); returns a 48x45 keyboard key cap."""
    w = 30 if len(label) == 1 else 44; x0 = (45 - w) // 2; y0, y1 = 9, 35; n = y1 - y0
    edge = [plate.getpixel((11, 16 + round(i * 13 / (n - 1)))) for i in range(n)]; face = [plate.getpixel((14, 16 + round(i * 13 / (n - 1)))) for i in range(n)]
    im = Image.new('RGBA', (48, 45), (0, 0, 0, 0))
    for i in range(n):
        for x in range(x0, x0 + w): im.putpixel((x, y0 + i), edge[i] if x in (x0, x0 + w - 1) else face[i])
    for x, y in ((x0, y0), (x0 + w - 1, y0), (x0, y1 - 1), (x0 + w - 1, y1 - 1)): im.putpixel((x, y), (0, 0, 0, 0))   # 四角各去一个像素 | remove one pixel at each of the four corners
    big = Image.new('RGBA', (48 * S, 45 * S), (0, 0, 0, 0)); g = ImageDraw.Draw(big)
    f = ImageFont.truetype(font, (19 if len(label) == 1 else 13 if len(label) == 3 else 10) * S); bb = g.textbbox((0, 0), label, font=f, stroke_width=S)
    g.text(((x0 + w / 2) * S - (bb[0] + bb[2]) / 2, (y0 + n / 2 - .5) * S - (bb[1] + bb[3]) / 2), label, font=f, fill=(255, 255, 255, 255), stroke_width=S, stroke_fill=(16, 16, 18, 255))
    im.alpha_composite(big.resize((48, 45), Image.LANCZOS))
    return im

def build(romfs, outdir):
    """返回 {原版键帽逻辑路径（小写）: 生成的贴图文件}，22 张齐全（没有键盘对应键的是原图字节）。 | Returns {logical path of the original key cap (lower case): generated texture file}, all 22 present (those with no keyboard counterpart are the original image bytes)."""
    L = GW.lib(); font = romfs_art.font(romfs, 'BlizzardGlobal-V5_81.ttf'); plate = romfs_art.sprite(romfs, DIR + 'BumperLeft.sprite'); repl = {}
    for line in open(os.path.join(romfs, 'data', 'build_manifest.psv'), encoding='utf-8'):
        f = line.rstrip('\n').split('|')
        if len(f) < 11 or line.startswith('#') or '/ui/controller/controllerbuttons/switch/' not in f[9].lower(): continue
        path = f[9].split(':', 1)[1]; name = os.path.basename(path)[:-len('.sprite')]; b = romfs_art.read(romfs, path); o = os.path.join(outdir, 'kbd_' + name + '.sprite')
        if name in KEYS:
            ver, fw, w, h = struct.unpack_from('<HHII', b, 4); assert b[:4] == b'SpA1' and ver == 61 and (w, h) == (48, 45), path
            b = GW.pack(L, b, GW.bc3(_cap(plate, KEYS[name], font)), w, h, fw)
        open(o, 'wb').write(b); repl[path.lower()] = o
    if len(repl) != ALL or sum(1 for k in repl if os.path.basename(k)[:-7] in {n.lower() for n in KEYS}) != len(KEYS): raise SystemExit('kbd_glyphs：手柄键图标应为 %d 张、其中 %d 张有键盘对应键，实际 %d 张（原包变了？）' % (ALL, len(KEYS), len(repl)))
    return repl


INPUT_BASES = tuple(range(128)) + tuple(range(256, 288)) + (512, 513)
UNIVERSAL_COUNT = len(INPUT_BASES) * 8 + 128 * 8 + 2


def _native_input_labels():
    """Consulta la misma API nativa que usa la ventana Controles, solo al construir."""
    override = os.environ.get('DM_KBD_INPUT_LABELS')
    if override:
        with open(override, encoding='utf-8') as stream:
            labels = {int(key): value for key, value in json.load(stream).items()}
    else:
        host = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'host'))
        source = r'''#import "dm_controls_mac.h"
#include <stdio.h>
int main(void) { @autoreleasepool {
    NSMutableDictionary *labels = [NSMutableDictionary dictionary];
    for (int mods = 0; mods < 8; mods++) for (int base = 0; base < 1024; base++) {
        if (!(base < 128 || (base >= 256 && base < 288) || base == 512 || base == 513)) continue;
        int input = base | mods << 10; char text[256];
        dm_controls_label(input, text, sizeof text);
        labels[[NSString stringWithFormat:@"%d", input]] = @(text);
    }
    NSData *data = [NSJSONSerialization dataWithJSONObject:labels options:0 error:nil];
    if (!data) return 1;
    fwrite(data.bytes, 1, data.length, stdout);
} }
'''
        with tempfile.TemporaryDirectory(prefix='d2r-glyph-labels-') as directory:
            helper = os.path.join(directory, 'labels.m')
            binary = os.path.join(directory, 'labels')
            with open(helper, 'w', encoding='utf-8') as stream:
                stream.write(source)
            subprocess.run(['clang', '-fobjc-arc', '-fblocks', '-I' + host, helper,
                            os.path.join(host, 'dm_controls_mac.m'), '-framework', 'Cocoa',
                            '-framework', 'Carbon', '-o', binary], check=True)
            labels = {int(key): value for key, value in json.loads(
                subprocess.check_output([binary], text=True)).items()}
    expected = {base | mods << 10 for mods in range(8) for base in INPUT_BASES}
    if set(labels) != expected or any(not isinstance(value, str) or not value for value in labels.values()):
        raise SystemExit('kbd_glyphs: las etiquetas nativas no cubren las 1296 entradas admitidas')
    for input, label in labels.items():
        if not label.isprintable():
            raise SystemExit('kbd_glyphs: etiqueta no imprimible de la entrada %d: %r' % (input, label))
    return labels


def _compact_label(label):
    replacements = {
        'Retorno': '↵', 'Espacio': 'Esp', 'Borrar': '⌫', 'Mayús': '⇧',
        'Mayús der.': 'MayD', '⌥ der.': 'OpD', '⌃ der.': 'CtrD',
        'Borrar num.': '⌫#', 'Intro num.': '↵#', '+ num.': '+#',
        'Inicio': 'Ini', 'RePág': 'Pg↑', 'AvPág': 'Pg↓', 'Ayuda': '?',
        'Ratón izquierdo': 'R1', 'Ratón derecho': 'R2',
        'Rueda arriba': 'R↑', 'Rueda abajo': 'R↓',
    }
    if label in replacements:
        return replacements[label]
    if label.startswith('Ratón '):
        return 'R' + label[6:]
    if label.startswith('Tecla '):
        return 'K' + label[6:]
    return label


def _universal_cap(plate, label, modifiers, font):
    # El tamaño y la piedra coinciden con los iconos originales de 48×45.
    image = _cap(plate, '', font)
    large = Image.new('RGBA', (48 * S, 45 * S), (0, 0, 0, 0))
    draw = ImageDraw.Draw(large)
    symbols = '/System/Library/Fonts/Apple Symbols.ttf'
    if not os.path.isfile(symbols):
        raise SystemExit('kbd_glyphs: falta la fuente de símbolos de macOS ' + symbols)
    rows = [(label, 27 if modifiers else 22, 12 if modifiers else 18)]
    if modifiers:
        prefix = ('⌃' if modifiers & 2 else '') + ('⌥' if modifiers & 4 else '') + ('⇧' if modifiers & 1 else '')
        rows.insert(0, (prefix, 13, 9))
    for text, center, height in rows:
        if any(c in '⌃⌥⇧⌘' for c in text):
            face = '/System/Library/Fonts/SFNS.ttf'
        else:
            face = symbols if any(c in '↵⌫←→↑↓↕↔' for c in text) else font
        selected = ImageFont.truetype(face, 48 * S)
        box = draw.textbbox((0, 0), text, font=selected, stroke_width=S // 2)
        ratio = min(40 * S / max(1, box[2] - box[0]), height * S / max(1, box[3] - box[1]))
        size = max(1, int(48 * S * ratio))
        while True:
            selected = ImageFont.truetype(face, size)
            box = draw.textbbox((0, 0), text, font=selected, stroke_width=S // 2)
            if box[2] - box[0] <= 40 * S and box[3] - box[1] <= height * S:
                break
            size -= 1
            if size <= 0:
                raise SystemExit('kbd_glyphs: la tecla no cabe en su icono: ' + text)
        draw.text((22 * S - (box[0] + box[2]) / 2, center * S - (box[1] + box[3]) / 2),
                  text, font=selected, fill=(255, 255, 255, 255),
                  stroke_width=S // 2, stroke_fill=(16, 16, 18, 255))
    image.alpha_composite(large.resize((48, 45), Image.Resampling.LANCZOS))
    return image


def build_universal(romfs, outdir):
    """Atlas completo para reasignaciones; no genera ni ejecuta nada durante el juego.

    Devuelve ({nombre .sprite: archivo}, manifest inputglyphs.tsv). Input%04x
    identifica la entrada real. Code%04x muestra el código físico si cambia la
    distribución y la nueva etiqueta no existe en el atlas. Unbound indica -1;
    Move es un indicador de movimiento sin letras fijas.
    """
    labels = _native_input_labels()
    library = GW.lib()
    font = romfs_art.font(romfs, 'BlizzardGlobal-V5_81.ttf')
    plate = romfs_art.sprite(romfs, DIR + 'BumperLeft.sprite')
    header = romfs_art.read(romfs, DIR + 'BumperLeft.sprite')
    version, display_width, width, height = struct.unpack_from('<HHII', header, 4)
    if header[:4] != b'SpA1' or version != 61 or (width, height) != (48, 45):
        raise SystemExit('kbd_glyphs: plantilla de sprite incompatible')
    result = {}
    def emit(name, label, modifiers):
        path = os.path.join(outdir, 'kbd_' + name + '.sprite')
        data = GW.pack(library, header, GW.bc3(_universal_cap(plate, label, modifiers, font)),
                       width, height, display_width)
        with open(path, 'wb') as stream:
            stream.write(data)
        result[name + '.sprite'] = path
    for input in sorted(labels):
        emit('Input%04x' % input, _compact_label(labels[input & 1023]), input >> 10)
        if (input & 1023) < 128:
            emit('Code%04x' % input, 'K%d' % (input & 1023), input >> 10)
    emit('Unbound', '—', 0)
    emit('Move', '↕↔', 0)
    if len(result) != UNIVERSAL_COUNT:
        raise SystemExit('kbd_glyphs: cobertura incompleta del atlas universal')
    manifest = os.path.join(outdir, 'inputglyphs.tsv')
    with open(manifest, 'w', encoding='utf-8', newline='\n') as stream:
        stream.write('# Entrada\tEtiqueta nativa completa; atlas generado al empaquetar\n')
        for input in sorted(labels):
            text = labels[input].replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t').replace('\r', '\\r')
            stream.write('%d\t%s\n' % (input, text))
    return result, manifest

if __name__ == '__main__':   # 预览：kbd_glyphs.py <资源目录> <输出目录>，另存一张放大的总览图 | preview: kbd_glyphs.py <resource directory> <output directory>, which also saves an enlarged overview image
    r = build(sys.argv[1], sys.argv[2]); L = GW.lib(); z = 6; sheet = Image.new('RGBA', (8 * 48 * z, 3 * 45 * z), (40, 40, 40, 255))
    for i, k in enumerate(sorted(r)): im = GW.decode(L, open(r[k], 'rb').read()); sheet.alpha_composite(im.resize((im.width * z, im.height * z), Image.NEAREST), ((i % 8) * 48 * z, (i // 8) * 45 * z))
    sheet.save(os.path.join(sys.argv[2], 'kbd_sheet.png')); print(len(r))
