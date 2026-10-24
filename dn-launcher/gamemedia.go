package main

// Launcher art out of the player's own game files. The repo ships none of the
// game's assets: the page's backgrounds, logo, icons and font are read from the
// installed client's paks (UE4 pak v3, unencrypted, zlib blocks) on first use,
// decoded (cooked Texture2D, DXT1/3/5 or BGRA8), scaled and cached under
// %LOCALAPPDATA%. No game files -> no art; the page falls back to plain colours.
// Same recipe as tools/pakx.py + tools/tex2png.py; the installer runs
// "--extract-media" once so the first launch has it all.

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"unicode/utf16"
)

// mediaVersion names the cache folder; bump it when the recipe below changes.
const mediaVersion = "1"

type mediaItem struct {
	asset string // path under the Content mount, without "../../../"
	w, h  int    // output size; h == 0 keeps the aspect ratio
	raw   bool   // copied byte for byte (fonts)
}

const ui = "DreadGame/Content/UserInterface/Textures/"
const loading = "DreadGame/Content/Generic/UI/LoadingScreens/DN_Loading_"

// The loading screens are 2048x2048 textures of a 16:9 picture, so they are
// stretched back to 1920x1080 rather than cropped.
var mediaManifest = map[string]mediaItem{
	"Quantico-Regular.ttf": {asset: "DreadGame/Content/Slate/Fonts/Quantico-Regular.ttf", raw: true},
	"Quantico-Bold.ttf":    {asset: "DreadGame/Content/Slate/Fonts/Quantico-Bold.ttf", raw: true},
	"logo.png":             {asset: ui + "Login/UI_Login_Logo.uasset", w: 560, h: 78},
	"credit.png":           {asset: "DreadGame/Content/Generic/UI/sRGB_icons/currencies/UI_Currency_SoftCurrency_Large.uasset", w: 128},
	"gold.png":             {asset: "DreadGame/Content/Generic/UI/sRGB_icons/currencies/UI_Currency_HardCurrency_Large.uasset", w: 128},
	"freexp.png":           {asset: "DreadGame/Content/Generic/UI/sRGB_icons/currencies/UI_Currency_FreeXP_Large.uasset", w: 128},
	"fl_akv.png":           {asset: ui + "ManufacturersScreen/UI_ManufacturersScreen_AkulaVektorLogo.uasset", w: 65, h: 88},
	"fl_jup.png":           {asset: ui + "ManufacturersScreen/UI_ManufacturersScreen_JupiterArmsLogo.uasset", w: 65, h: 88},
	"fl_obe.png":           {asset: ui + "ManufacturersScreen/UI_ManufacturersScreen_OberonLogo.uasset", w: 65, h: 88},
	"fl_t1.png":            {asset: ui + "SelectFleet/UI_SelectFleet_IconRecruit.uasset", w: 67, h: 96},
	"fl_t2.png":            {asset: ui + "SelectFleet/UI_SelectFleet_IconVeteran.uasset", w: 72, h: 96},
	"fl_t3.png":            {asset: ui + "SelectFleet/UI_SelectFleet_IconLegendary.uasset", w: 68, h: 96},
	"ArtilleryCruiser.png": {asset: ui + "ShipIcons/UI_ShipClass_ArtilleryCruiser.uasset", w: 64, h: 64},
	"Corvette.png":         {asset: ui + "ShipIcons/UI_ShipClass_Corvette.uasset", w: 64, h: 64},
	"Destroyer.png":        {asset: ui + "ShipIcons/UI_ShipClass_Destroyer.uasset", w: 64, h: 64},
	"Dreadnought.png":      {asset: ui + "ShipIcons/UI_ShipClass_Dreadnought.uasset", w: 64, h: 64},
	"TacticalCruiser.png":  {asset: ui + "ShipIcons/UI_ShipClass_TacticalCruiser.uasset", w: 64, h: 64},
}

func init() {
	for _, m := range []string{"Amirani", "Elysium", "Glacier", "Gorge", "Highlands", "Skybridge", "Space01", "Space02"} {
		mediaManifest["bg_"+strings.ToLower(m)+".jpg"] = mediaItem{asset: loading + m + ".uasset", w: 1920, h: 1080}
		night := m + "_Night"
		if m == "Space02" {
			night = m + "_night" // the client's own spelling
		}
		mediaManifest["bg_"+strings.ToLower(m)+"_night.jpg"] = mediaItem{asset: loading + night + ".uasset", w: 1920, h: 1080}
	}
	for _, m := range []string{"Derelict", "Thebe", "Login"} {
		mediaManifest["bg_"+strings.ToLower(m)+".jpg"] = mediaItem{asset: loading + m + ".uasset", w: 1920, h: 1080}
	}
}

// mediaNames lists the manifest, sorted.
func mediaNames() []string {
	var out []string
	for k := range mediaManifest {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

// gameMedia serves the manifest from a cache folder, extracting a file from
// the paks the first time it is asked for.
type gameMedia struct {
	paks  func() string // the client's Content/Paks folder, "" if unknown
	cache string

	mu    sync.Mutex
	index map[string]pakEntry
	dir   string
}

func newGameMedia(cacheRoot string, paks func() string) *gameMedia {
	return &gameMedia{paks: paks, cache: filepath.Join(cacheRoot, "media", "v"+mediaVersion)}
}

// Get returns the named file's bytes, from the cache or freshly extracted.
func (g *gameMedia) Get(name string) ([]byte, error) {
	item, ok := mediaManifest[name]
	if !ok {
		return nil, os.ErrNotExist
	}
	dst := filepath.Join(g.cache, name)
	//nolint:gosec // name is a manifest key; the cache is the launcher's own folder.
	if b, err := os.ReadFile(dst); err == nil && len(b) > 0 {
		return b, nil
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	if err := g.loadIndex(); err != nil {
		return nil, err
	}
	b, err := g.extract(name, item)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", name, err)
	}
	if err := os.MkdirAll(g.cache, 0o700); err == nil {
		tmp := dst + ".tmp"
		if os.WriteFile(tmp, b, 0o600) == nil {
			_ = os.Rename(tmp, dst)
		}
	}
	return b, nil
}

// Available reports whether the game files (or a cached copy) can supply name.
func (g *gameMedia) Available(name string) bool {
	if _, ok := mediaManifest[name]; !ok {
		return false
	}
	if st, err := os.Stat(filepath.Join(g.cache, name)); err == nil && st.Size() > 0 {
		return true
	}
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.loadIndex() != nil {
		return false
	}
	_, ok := g.index[mediaManifest[name].asset]
	return ok
}

// ExtractAll fills the cache; the installer's step. Returns how many files
// were written and the first error.
func (g *gameMedia) ExtractAll() (int, error) {
	n := 0
	var first error
	for _, name := range mediaNames() {
		if _, err := g.Get(name); err != nil {
			if first == nil {
				first = err
			}
			continue
		}
		n++
	}
	return n, first
}

func (g *gameMedia) loadIndex() error {
	dir := g.paks()
	if dir == "" {
		return errors.New("game files not found")
	}
	if g.index != nil && g.dir == dir {
		return nil
	}
	idx, err := pakIndex(dir)
	if err != nil {
		return err
	}
	g.index, g.dir = idx, dir
	return nil
}

func (g *gameMedia) extract(name string, item mediaItem) ([]byte, error) {
	e, ok := g.index[item.asset]
	if !ok {
		return nil, errors.New("not in the game files")
	}
	raw, err := e.read()
	if err != nil {
		return nil, err
	}
	if item.raw {
		return raw, nil
	}
	img, err := decodeTexture(raw)
	if err != nil {
		return nil, err
	}
	w, h := item.w, item.h
	if h == 0 {
		b := img.Bounds()
		h = (b.Dy()*w + b.Dx()/2) / b.Dx()
	}
	img = resample(img, w, h)
	var buf bytes.Buffer
	if strings.HasSuffix(name, ".jpg") {
		err = jpeg.Encode(&buf, img, &jpeg.Options{Quality: 90})
	} else {
		err = png.Encode(&buf, img)
	}
	return buf.Bytes(), err
}

// ---- pak v3 ----------------------------------------------------------

type pakEntry struct {
	pak  string
	off  int64
	size int64
	comp int32
}

const pakMagic = 0x5A6F12E1

// pakIndex maps "DreadGame/Content/..." paths to entries across every pak in
// dir. Where two paks hold the same path the later one (by name) wins, as the
// engine's patch paks do.
func pakIndex(dir string) (map[string]pakEntry, error) {
	paks, _ := filepath.Glob(filepath.Join(dir, "*.pak"))
	if len(paks) == 0 {
		return nil, errors.New("no .pak files in " + dir)
	}
	sort.Strings(paks)
	out := map[string]pakEntry{}
	for _, p := range paks {
		if err := readPakIndex(p, out); err != nil {
			return nil, fmt.Errorf("%s: %w", filepath.Base(p), err)
		}
	}
	return out, nil
}

func readPakIndex(path string, out map[string]pakEntry) error {
	//nolint:gosec // the game's own pak files.
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer func() { _ = f.Close() }()
	var foot [24]byte
	if _, err := f.Seek(-44, io.SeekEnd); err != nil {
		return err
	}
	if _, err := io.ReadFull(f, foot[:]); err != nil {
		return err
	}
	if binary.LittleEndian.Uint32(foot[0:]) != pakMagic {
		return errors.New("not a pak")
	}
	ioff := int64(binary.LittleEndian.Uint64(foot[8:]))
	isize := int64(binary.LittleEndian.Uint64(foot[16:]))
	if isize <= 0 || isize > 256<<20 {
		return errors.New("bad index size")
	}
	idx := make([]byte, isize)
	if _, err := f.ReadAt(idx, ioff); err != nil {
		return err
	}
	r := &byteReader{b: idx}
	mount := strings.TrimLeft(r.fstring(), "./")
	n := int(r.i32())
	for i := 0; i < n && r.err == nil; i++ {
		name := r.fstring()
		e := pakEntry{pak: path}
		e.off, e.size = r.i64(), r.i64()
		r.i64() // uncompressed size
		e.comp = r.i32()
		r.skip(20) // sha1
		if e.comp != 0 {
			r.skip(int(r.i32()) * 16)
		}
		r.skip(1 + 4) // encrypted, block size
		if r.err == nil {
			out[mount+name] = e
		}
	}
	return r.err
}

// read returns the entry's bytes. The record repeats its header at off; in
// v3 the compression blocks hold absolute offsets.
func (e pakEntry) read() ([]byte, error) {
	//nolint:gosec // the game's own pak files.
	f, err := os.Open(e.pak)
	if err != nil {
		return nil, err
	}
	defer func() { _ = f.Close() }()
	hdr := make([]byte, 28+20+4)
	if _, err := f.ReadAt(hdr, e.off); err != nil {
		return nil, err
	}
	hr := &byteReader{b: hdr}
	hr.skip(8)
	size := hr.i64()
	usize := hr.i64()
	comp := hr.i32()
	if size < 0 || usize < 0 || usize > 512<<20 {
		return nil, errors.New("bad entry")
	}
	if comp == 0 {
		b := make([]byte, size)
		_, err := f.ReadAt(b, e.off+28+20+1+4)
		return b, err
	}
	nb := int(binary.LittleEndian.Uint32(hdr[48:]))
	if nb <= 0 || nb > 1<<16 {
		return nil, errors.New("bad block count")
	}
	blk := make([]byte, nb*16)
	if _, err := f.ReadAt(blk, e.off+52); err != nil {
		return nil, err
	}
	out := make([]byte, 0, usize)
	for i := 0; i < nb; i++ {
		s := int64(binary.LittleEndian.Uint64(blk[i*16:]))
		t := int64(binary.LittleEndian.Uint64(blk[i*16+8:]))
		if t < s || t-s > 16<<20 {
			return nil, errors.New("bad block")
		}
		zr, err := zlib.NewReader(io.NewSectionReader(f, s, t-s))
		if err != nil {
			return nil, err
		}
		b, err := io.ReadAll(zr)
		_ = zr.Close()
		if err != nil {
			return nil, err
		}
		out = append(out, b...)
	}
	return out, nil
}

type byteReader struct {
	b   []byte
	p   int
	err error
}

func (r *byteReader) take(n int) []byte {
	if r.err != nil || n < 0 || r.p+n > len(r.b) {
		r.err = io.ErrUnexpectedEOF
		return make([]byte, max(n, 0))
	}
	s := r.b[r.p : r.p+n]
	r.p += n
	return s
}
func (r *byteReader) skip(n int) { r.take(n) }
func (r *byteReader) i32() int32 { return int32(binary.LittleEndian.Uint32(r.take(4))) }
func (r *byteReader) i64() int64 { return int64(binary.LittleEndian.Uint64(r.take(8))) }
func (r *byteReader) fstring() string {
	n := int(r.i32())
	switch {
	case n == 0:
		return ""
	case n < 0:
		b := r.take(-n * 2)
		u := make([]uint16, -n)
		for i := range u {
			u[i] = binary.LittleEndian.Uint16(b[i*2:])
		}
		return strings.TrimRight(string(utf16.Decode(u)), "\x00")
	default:
		return strings.TrimRight(string(r.take(n)), "\x00")
	}
}

// ---- cooked Texture2D (UE 4.13) ----------------------------------------

// decodeTexture finds FTexturePlatformData by its pixel-format FString:
// SizeX, SizeY, NumSlices precede it; FirstMip, NumMips and the mips follow,
// each a bCooked i32, a bulk-data header (flags u32, count i32, size i32,
// offset i64), inline data unless stored elsewhere, then the mip's size. The
// name table holds the same string, so a match needs sane sizes before it.
// Flag 0x1 (payload at end of file) puts the bytes at the file's end, before
// the trailing package tag. The first mip with data is used.
func decodeTexture(raw []byte) (image.Image, error) {
	for _, pf := range []string{"PF_DXT1", "PF_DXT5", "PF_DXT3", "PF_B8G8R8A8"} {
		key := make([]byte, 4, 4+len(pf)+1)
		binary.LittleEndian.PutUint32(key, uint32(len(pf)+1))
		key = append(append(key, pf...), 0)
		i, from := -1, 0
		for {
			j := bytes.Index(raw[from:], key)
			if j < 0 {
				break
			}
			j += from
			if j >= 12 {
				sx := int32(binary.LittleEndian.Uint32(raw[j-12:]))
				sy := int32(binary.LittleEndian.Uint32(raw[j-8:]))
				ns := int32(binary.LittleEndian.Uint32(raw[j-4:]))
				if sx > 0 && sx <= 8192 && sy > 0 && sy <= 8192 && ns == 1 {
					i = j
					break
				}
			}
			from = j + 1
		}
		if i < 0 {
			continue
		}
		r := &byteReader{b: raw, p: i + len(key)}
		r.i32() // first mip
		nmips := int(r.i32())
		type mip struct {
			flags uint32
			size  int
			off   int64
			data  []byte
			w, h  int
		}
		var mips []mip
		for k := 0; k < nmips && k < 16 && r.err == nil; k++ {
			r.i32() // cooked
			m := mip{flags: uint32(r.i32())}
			r.i32() // element count
			m.size = int(r.i32())
			m.off = r.i64()
			if m.flags&0x121 == 0 && m.size > 0 && r.p+m.size <= len(raw) {
				m.data = r.take(m.size)
			}
			m.w, m.h = int(r.i32()), int(r.i32())
			mips = append(mips, m)
			if m.w <= 0 || m.w > 8192 || m.h <= 0 || m.h > 8192 {
				break
			}
		}
		var tail int64
		for _, m := range mips {
			if m.flags&0x1 != 0 && m.flags&0x120 == 0 && m.size > 0 && m.off+int64(m.size) > tail {
				tail = m.off + int64(m.size)
			}
		}
		bulk := int64(len(raw)) - 4 - tail
		for _, m := range mips {
			if m.data == nil && tail > 0 && m.flags&0x1 != 0 && m.flags&0x120 == 0 && m.size > 0 && bulk+m.off >= 0 && bulk+m.off+int64(m.size) <= int64(len(raw)) {
				m.data = raw[bulk+m.off : bulk+m.off+int64(m.size)]
			}
			if m.data != nil && m.w > 0 && m.w <= 8192 && m.h > 0 && m.h <= 8192 {
				return decodePixels(pf, m.w, m.h, m.data)
			}
		}
		return nil, fmt.Errorf("no mip data (%d mips)", len(mips))
	}
	return nil, errors.New("no known pixel format")
}

func decodePixels(pf string, w, h int, data []byte) (image.Image, error) {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	if pf == "PF_B8G8R8A8" {
		if len(data) < w*h*4 {
			return nil, errors.New("short BGRA data")
		}
		for i := 0; i < w*h; i++ {
			img.Pix[i*4], img.Pix[i*4+1], img.Pix[i*4+2], img.Pix[i*4+3] = data[i*4+2], data[i*4+1], data[i*4], data[i*4+3]
		}
		return img, nil
	}
	bs := 16
	if pf == "PF_DXT1" {
		bs = 8
	}
	bw, bh := (w+3)/4, (h+3)/4
	if len(data) < bw*bh*bs {
		return nil, errors.New("short DXT data")
	}
	var px [16]color.NRGBA
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			b := data[(by*bw+bx)*bs:]
			switch pf {
			case "PF_DXT1":
				dxtColor(b, &px, true)
			case "PF_DXT3":
				dxtColor(b[8:], &px, false)
				for k := 0; k < 16; k++ {
					a := b[k/2] >> (4 * (k % 2)) & 0xF
					px[k].A = a<<4 | a
				}
			case "PF_DXT5":
				dxtColor(b[8:], &px, false)
				dxt5Alpha(b, &px)
			}
			for k := 0; k < 16; k++ {
				x, y := bx*4+k%4, by*4+k/4
				if x < w && y < h {
					img.SetNRGBA(x, y, px[k])
				}
			}
		}
	}
	return img, nil
}

func rgb565(v uint16) [3]int {
	r, g, b := int(v>>11&31), int(v>>5&63), int(v&31)
	return [3]int{r<<3 | r>>2, g<<2 | g>>4, b<<3 | b>>2}
}

func dxtColor(b []byte, px *[16]color.NRGBA, dxt1 bool) {
	c0v, c1v := binary.LittleEndian.Uint16(b), binary.LittleEndian.Uint16(b[2:])
	c0, c1 := rgb565(c0v), rgb565(c1v)
	var pal [4]color.NRGBA
	pal[0] = color.NRGBA{uint8(c0[0]), uint8(c0[1]), uint8(c0[2]), 255}
	pal[1] = color.NRGBA{uint8(c1[0]), uint8(c1[1]), uint8(c1[2]), 255}
	mix := func(a, b, wa, wb, d int) uint8 { return uint8((a*wa + b*wb) / d) }
	if c0v > c1v || !dxt1 {
		for i := 0; i < 3; i++ {
			v2 := mix(c0[i], c1[i], 2, 1, 3)
			v3 := mix(c0[i], c1[i], 1, 2, 3)
			setc(&pal[2], i, v2)
			setc(&pal[3], i, v3)
		}
		pal[2].A, pal[3].A = 255, 255
	} else {
		for i := 0; i < 3; i++ {
			setc(&pal[2], i, mix(c0[i], c1[i], 1, 1, 2))
		}
		pal[2].A = 255
		pal[3] = color.NRGBA{}
	}
	bits := binary.LittleEndian.Uint32(b[4:])
	for k := 0; k < 16; k++ {
		px[k] = pal[bits>>(2*k)&3]
	}
}

func setc(c *color.NRGBA, i int, v uint8) {
	switch i {
	case 0:
		c.R = v
	case 1:
		c.G = v
	default:
		c.B = v
	}
}

func dxt5Alpha(b []byte, px *[16]color.NRGBA) {
	a0, a1 := int(b[0]), int(b[1])
	var pal [8]int
	pal[0], pal[1] = a0, a1
	if a0 > a1 {
		for i := 1; i < 7; i++ {
			pal[i+1] = ((7-i)*a0 + i*a1) / 7
		}
	} else {
		for i := 1; i < 5; i++ {
			pal[i+1] = ((5-i)*a0 + i*a1) / 5
		}
		pal[6], pal[7] = 0, 255
	}
	var bits uint64
	for i := 0; i < 6; i++ {
		bits |= uint64(b[2+i]) << (8 * i)
	}
	for k := 0; k < 16; k++ {
		px[k].A = uint8(pal[bits>>(3*k)&7])
	}
}

// ---- scaling ---------------------------------------------------------

// resample scales by area averaging (every source pixel weighted by how much
// of it falls in the destination pixel), in premultiplied alpha so clear
// pixels don't darken the edges of the icons.
func resample(src image.Image, w, h int) image.Image {
	sb := src.Bounds()
	sw, sh := sb.Dx(), sb.Dy()
	if sw == w && sh == h {
		return src
	}
	in := make([]float32, sw*sh*4)
	for y := 0; y < sh; y++ {
		for x := 0; x < sw; x++ {
			c := color.NRGBAModel.Convert(src.At(sb.Min.X+x, sb.Min.Y+y)).(color.NRGBA)
			a := float32(c.A) / 255
			i := (y*sw + x) * 4
			in[i], in[i+1], in[i+2], in[i+3] = float32(c.R)*a, float32(c.G)*a, float32(c.B)*a, float32(c.A)
		}
	}
	tmp := make([]float32, w*sh*4)
	for y := 0; y < sh; y++ {
		area1D(in[y*sw*4:], 4, sw, tmp[y*w*4:], 4, w)
	}
	out := make([]float32, w*h*4)
	for x := 0; x < w; x++ {
		area1D(tmp[x*4:], w*4, sh, out[x*4:], w*4, h)
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for i := 0; i < w*h; i++ {
		a := out[i*4+3]
		if a < 0.5 {
			continue
		}
		k := 255 / a
		img.Pix[i*4] = clamp8(out[i*4] * k)
		img.Pix[i*4+1] = clamp8(out[i*4+1] * k)
		img.Pix[i*4+2] = clamp8(out[i*4+2] * k)
		img.Pix[i*4+3] = clamp8(a)
	}
	return img
}

// area1D resamples n source samples (stride ss, 4 channels each) into m.
func area1D(src []float32, ss, n int, dst []float32, ds, m int) {
	scale := float64(n) / float64(m)
	for j := 0; j < m; j++ {
		lo, hi := float64(j)*scale, float64(j+1)*scale
		var acc [4]float64
		for i := int(lo); i < n && float64(i) < hi; i++ {
			wt := min(hi, float64(i+1)) - max(lo, float64(i))
			for c := 0; c < 4; c++ {
				acc[c] += float64(src[i*ss+c]) * wt
			}
		}
		for c := 0; c < 4; c++ {
			dst[j*ds+c] = float32(acc[c] / scale)
		}
	}
}

func clamp8(v float32) uint8 {
	switch {
	case v <= 0:
		return 0
	case v >= 255:
		return 255
	}
	return uint8(v + 0.5)
}
