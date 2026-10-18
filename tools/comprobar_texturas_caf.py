"""Check saved CAF textures (comprobar = check); never modify game/save data.

Requires the already-provided Pillow, also used by the screenshot analysis tools.
Run with --self-test, or pass one or more CAFTEX.DAT files. Exit 1 means at least
one checked image is entirely transparent black; it is not a shading test.
Use --all-mips to also check every saved mip, not just the eight base images.
--mip-bands reports full-width black RGB bands that contain color in the next mip.
These are content inconsistencies to inspect, not an automatic shading verdict.
"""
import argparse
import hashlib
import io
from pathlib import Path
import struct

from PIL import Image, ImageStat


def extraer(datos):
    """Read the DDS chain in UFC3's observed 64-byte CAFTEX wrapper."""
    if len(datos) < 64:
        raise ValueError("Truncated CAFTEX header")
    cantidad = int.from_bytes(datos[32:36], "big")
    if not 1 <= cantidad <= 8:
        raise ValueError("Invalid CAFTEX image count")
    posicion = 64
    imagenes = []
    for _ in range(cantidad):
        if posicion + 128 > len(datos) or datos[posicion:posicion + 4] != b"DDS ":
            raise ValueError(f"Missing DDS header at {posicion:#x}")
        cabecera = struct.unpack_from("<31I", datos, posicion + 4)
        alto, ancho, niveles = cabecera[2], cabecera[3], max(1, cabecera[6])
        formato = datos[posicion + 84:posicion + 88]
        if (cabecera[0] != 124 or cabecera[18] != 32 or formato != b"DXT5"
                or not 1 <= ancho <= 8192 or not 1 <= alto <= 8192
                or not 1 <= niveles <= 14):
            raise ValueError(f"Unsupported DDS layout at {posicion:#x}")
        largo = sum(max(1, ((ancho >> i) + 3) // 4)
                    * max(1, ((alto >> i) + 3) // 4) * 16 for i in range(niveles))
        fin = posicion + 128 + largo
        if fin > len(datos):
            raise ValueError(f"Truncated DDS payload at {posicion:#x}")
        imagenes.append((posicion, datos[posicion:fin]))
        posicion = fin
    if posicion != len(datos):
        raise ValueError("Unexpected data after the DDS chain")
    return imagenes


def estadisticas(dds):
    with Image.open(io.BytesIO(dds)) as imagen:
        rgba = imagen.convert("RGBA")
        vacia = all(maximo == 0 for _, maximo in rgba.getextrema())
        return rgba.size, vacia, ImageStat.Stat(rgba.convert("RGB")).mean


def comprobar(datos):
    return [(posicion, *estadisticas(dds)) for posicion, dds in extraer(datos)]


def iterar_mips(datos):
    """Yield each BC3 mip independently; Pillow's DDS loader opens only mip 0."""
    for atlas, (posicion, dds) in enumerate(extraer(datos)):
        alto, ancho = struct.unpack_from("<II", dds, 12)
        niveles = max(1, struct.unpack_from("<I", dds, 28)[0])
        inicio = 128
        for nivel in range(niveles):
            w, h = max(1, ancho >> nivel), max(1, alto >> nivel)
            largo = ((w + 3) // 4) * ((h + 3) // 4) * 16
            cabecera = bytearray(dds[:128])
            struct.pack_into("<III", cabecera, 12, h, w, largo)
            struct.pack_into("<I", cabecera, 28, 1)
            mip = cabecera + dds[inicio:inicio + largo]
            yield atlas, nivel, posicion + inicio, bytes(mip)
            inicio += largo


def comprobar_mips(datos):
    return [(atlas, nivel, offset, *estadisticas(mip))
            for atlas, nivel, offset, mip in iterar_mips(datos)]


def bandas_negras(imagen):
    """Find maximal, full-width bands with zero RGB, including opaque black."""
    rgb = imagen.convert("RGB")
    inicio = None
    for y in range(rgb.height + 1):
        negra = y < rgb.height and rgb.crop((0, y, rgb.width, y + 1)).getbbox() is None
        if negra and inicio is None:
            inicio = y
        elif not negra and inicio is not None:
            yield inicio, y
            inicio = None


def comprobar_bandas(datos):
    anterior = None
    for atlas, nivel, _, mip in iterar_mips(datos):
        with Image.open(io.BytesIO(mip)) as fuente:
            imagen = fuente.convert("RGB")
        if anterior is not None and anterior[0] == atlas:
            _, nivel_anterior, previa = anterior
            for inicio, fin in bandas_negras(previa):
                # Conservative corresponding coverage, including odd mip sizes.
                y0 = inicio * imagen.height // previa.height
                y1 = (fin * imagen.height + previa.height - 1) // previa.height
                if imagen.crop((0, y0, imagen.width, y1)).getbbox() is not None:
                    yield atlas, nivel_anterior, inicio, fin
        anterior = atlas, nivel, imagen


def autocontrol():
    cabecera = bytearray(128)
    cabecera[:4] = b"DDS "
    for offset, valor in ((4, 124), (8, 0x81007), (12, 4), (16, 4),
                          (20, 16), (28, 1), (76, 32), (80, 4), (108, 0x1000)):
        struct.pack_into("<I", cabecera, offset, valor)
    cabecera[84:88] = b"DXT5"
    envoltura = bytearray(64)
    struct.pack_into(">I", envoltura, 32, 1)
    negro = bytes.fromhex("00004992244992244108000055555555")
    assert comprobar(envoltura + cabecera + negro)[0][2]
    # BC3 always has four RGB entries, including when endpoint0 <= endpoint1.
    color = bytes([255, 255]) + bytes(6) + struct.pack("<HHI", 0x001F, 0xF800, 0xFFFFFFFF)
    fila = comprobar(envoltura + cabecera + color)[0]
    assert not fila[2] and fila[3] == [170.0, 0.0, 85.0], fila
    # A valid base must not hide a black lower mip (4x4 -> 2x2 -> 1x1).
    struct.pack_into("<I", cabecera, 28, 3)
    mips = comprobar_mips(envoltura + cabecera + color + negro + color)
    assert [m[3] for m in mips] == [(4, 4), (2, 2), (1, 1)]
    assert [m[4] for m in mips] == [False, True, False]
    assert [m[2] for m in mips] == [192, 208, 224]
    assert list(comprobar_bandas(envoltura + cabecera + color + negro + color)) == [(0, 1, 0, 2)]
    opaque = Image.new("RGBA", (8, 8), (0, 0, 0, 255))
    opaque.paste((20, 30, 40, 255), (0, 0, 8, 2))
    opaque.paste((20, 30, 40, 255), (0, 6, 8, 8))
    assert list(bandas_negras(opaque)) == [(2, 6)]
    assert list(bandas_negras(Image.new("RGB", (4, 4), "white"))) == []
    struct.pack_into("<I", cabecera, 28, 1)
    try:
        extraer(envoltura + cabecera + negro[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError("Truncated DDS must fail")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archivos", type=Path, nargs="*")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--all-mips", action="store_true")
    parser.add_argument("--mip-bands", action="store_true")
    args = parser.parse_args()
    if not args.archivos and not args.self_test:
        parser.error("Pass CAFTEX.DAT paths or --self-test")
    if args.self_test:
        autocontrol()
        print("CAFTEX parser and independent BC3 decoder self-check: OK")
    hay_vacias = False
    for archivo in args.archivos:
        datos = archivo.read_bytes()
        print(f"{archivo}: SHA256={hashlib.sha256(datos).hexdigest()}")
        filas = (comprobar_mips(datos) if args.all_mips else
                 [(i, 0, *fila) for i, fila in enumerate(comprobar(datos))])
        for i, nivel, offset, tamano, vacia, media in filas:
            hay_vacias |= vacia
            print(f"  {i} mip={nivel}: offset={offset:#x} size={tamano[0]}x{tamano[1]} "
                  f"transparent_black={vacia} mean_RGB={tuple(round(v, 3) for v in media)}")
        if args.mip_bands:
            for atlas, nivel, inicio, fin in comprobar_bandas(datos):
                print(f"  MIP_BAND atlas={atlas} mip={nivel} black_RGB_rows=[{inicio},{fin}) "
                      "next_mip_has_color=true")
    return int(hay_vacias)


if __name__ == "__main__":
    raise SystemExit(main())
