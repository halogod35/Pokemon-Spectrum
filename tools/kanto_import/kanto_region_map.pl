#!/usr/bin/perl
# tools/kanto_import/kanto_region_map.pl -- PS-095: the Kanto picture of the Fly / PokéNav / wall map,
# converted from pokefirered's region map graphics into Emerald's form (docs/regions.md step 6).
#
#   perl tools/kanto_import/kanto_region_map.pl --frlg <pokefirered checkout> [--repo <root>] [--preview <png>]
#
# pokefirered draws its Kanto map on a text background: graphics/region_map/kanto.bin is a 30x20
# tilemap of 16-bit entries (tile id, palette 0..4, flips) over 4bpp tiles (region_map.png, an 8-bit PNG whose
# pixel values carry the palette number too: gbagfx keeps their low four bits) and an
# 80-colour palette (region_map.pal); the cursor cell (gx, gy) of its 22x15 layout sits on tile
# (gx + 4, gy + 4). Emerald's region map is an affine background that zooms: a 64x64 tilemap of
# 8-bit tile ids (graphics/pokenav/region_map/map.bin), 8bpp tiles whose pixels index the whole
# background palette, 48 colours loaded at BG_PLTT_ID(7) (src/region_map.c), and the cursor cell
# (gx, gy) on tile (gx + 1, gy + 2) (MAPCURSOR_X_MIN / Y_MIN). The map sections keep pokefirered's
# cell coordinates (tools/kanto_import/kanto_import.pl copies them), so this script places FRLG's
# tile (fx, fy) at Emerald's (fx - 3, fy - 2): the picture body (FRLG tiles 3..26 x 2..19, 24x18)
# lands at 0..23 x 0..17 and every cursor cell lands on the same piece of the picture as in FRLG.
# FRLG's frame columns (palette 2) and its SWITCH / CANCEL buttons (palette 3) are replaced by open
# sea; everything outside the body is open sea too (Emerald tile 0, like Hoenn's map.bin).
#
# Writes graphics/pokenav/region_map/kanto_map.png (8bpp indexed, pixel = 0x70 + colour slot),
# kanto_map.pal (JASC, 48 colours) and kanto_map.bin (64x64 bytes); src/region.c includes them
# with INCGFX (no -num_tiles: the png holds exactly the tiles, padded to a full 16-tile row).
# Perl core modules only; the pokefirered files are data, nothing is executed from that checkout.
use strict;
use warnings;
use Compress::Zlib ();
use File::Basename qw(dirname);
use File::Spec;
use Getopt::Long;

my $FR_MAP_W = 30;              # kanto.bin
my $FR_MAP_H = 20;
my $FR_BODY_X0 = 3;             # first / last picture column and row in kanto.bin (inside the frame)
my $FR_BODY_X1 = 26;
my $FR_BODY_Y0 = 2;
my $FR_BODY_Y1 = 19;
my $FR_CURSOR_OFFSET = 4;       # pokefirered region_map.c: sprite x = 8 * cursorX + 36 (16 px cursor centred on tile cursorX + 4)
my $EM_CURSOR_X_MIN = 1;        # src/region_map.c MAPCURSOR_X_MIN / Y_MIN
my $EM_CURSOR_Y_MIN = 2;
my $EM_TILEMAP_SIDE = 64;       # 64x64 bytes
my $EM_MAX_TILES = 256;         # 8-bit tile ids
my $EM_PAL_BASE = 0x70;         # BG_PLTT_ID(7): LoadPalette(mapPalette, BG_PLTT_ID(7), 3 * PLTT_SIZE_4BPP)
my $EM_MAX_COLOURS = 48;
my $FR_PICTURE_PALETTES = 2;    # kanto.bin palettes 0 and 1 draw the land and sea; 2 is the frame, 3 the buttons
my $PNG_WIDTH = 128;            # 16 tiles per row, like every tileset / region map png here
my $OFF_X = $EM_CURSOR_X_MIN - $FR_CURSOR_OFFSET;   # FRLG tile -> Emerald tile: -3
my $OFF_Y = $EM_CURSOR_Y_MIN - $FR_CURSOR_OFFSET;   # -2

my ($frlg, $repo, $preview);
GetOptions('frlg=s' => \$frlg, 'repo=s' => \$repo, 'preview=s' => \$preview)
    or die "usage: $0 --frlg <pokefirered> [--repo <root>] [--preview <png>]\n";
die "--frlg <pokefirered checkout> is required\n" unless defined $frlg && -f "$frlg/graphics/region_map/kanto.bin";
$repo //= File::Spec->rel2abs(File::Spec->catdir(dirname(__FILE__), '..', '..'));
my $outDir = "$repo/graphics/pokenav/region_map";
die "repo root $repo has no $outDir\n" unless -d $outDir;

sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; return $d }
sub spew  { my ($f, $d) = @_; open my $h, '>', $f or die "cannot write $f: $!\n"; binmode $h; print $h $d; close $h }

# ---------------------------------------------------------------------------------------------
# PNG in (indexed, 4 or 8 bits, not interlaced) / out (8-bit indexed)
# ---------------------------------------------------------------------------------------------
sub png_read
{
    my $path = shift;
    my $d = slurp($path);
    die "$path: not a PNG\n" unless substr($d, 0, 8) eq "\x89PNG\r\n\x1a\n";
    my ($pos, %ihdr, $plte, $idat) = (8, (), '', '');
    while ($pos + 8 <= length $d)
    {
        my ($len, $type) = unpack('N a4', substr($d, $pos, 8));
        my $data = substr($d, $pos + 8, $len);
        if    ($type eq 'IHDR') { @ihdr{qw(w h bd ct cm fl il)} = unpack('N N C C C C C', $data) }
        elsif ($type eq 'PLTE') { $plte = $data }
        elsif ($type eq 'IDAT') { $idat .= $data }
        $pos += 12 + $len;
        last if $type eq 'IEND';
    }
    die "$path: expected an indexed, non-interlaced PNG with 4 or 8 bits per pixel\n"
        unless $ihdr{ct} == 3 && $ihdr{il} == 0 && ($ihdr{bd} == 4 || $ihdr{bd} == 8);
    my $raw = Compress::Zlib::uncompress($idat);
    die "$path: zlib inflate failed\n" unless defined $raw;
    my $stride = $ihdr{bd} == 8 ? $ihdr{w} : ($ihdr{w} + 1) >> 1;
    die "$path: IDAT size mismatch\n" unless length($raw) == $ihdr{h} * ($stride + 1);
    my (@rows, @prev);
    @prev = (0) x $stride;
    for my $y (0 .. $ihdr{h} - 1)
    {
        my $filter = ord(substr($raw, $y * ($stride + 1), 1));
        my @x = unpack('C*', substr($raw, $y * ($stride + 1) + 1, $stride));
        for my $i (0 .. $stride - 1)
        {
            my $a = $i > 0 ? $x[$i - 1] : 0;
            my $b = $prev[$i];
            my $c = $i > 0 ? $prev[$i - 1] : 0;
            if    ($filter == 0) { }
            elsif ($filter == 1) { $x[$i] = ($x[$i] + $a) & 0xFF }
            elsif ($filter == 2) { $x[$i] = ($x[$i] + $b) & 0xFF }
            elsif ($filter == 3) { $x[$i] = ($x[$i] + int(($a + $b) / 2)) & 0xFF }
            elsif ($filter == 4)
            {
                my $p = $a + $b - $c;
                my ($pa, $pb, $pc) = (abs($p - $a), abs($p - $b), abs($p - $c));
                my $pred = ($pa <= $pb && $pa <= $pc) ? $a : ($pb <= $pc) ? $b : $c;
                $x[$i] = ($x[$i] + $pred) & 0xFF;
            }
            else { die "$path: unknown PNG filter $filter\n" }
        }
        @prev = @x;
        if ($ihdr{bd} == 8) { push @rows, \@x }
        else { my @px; for my $byte (@x) { push @px, $byte >> 4, $byte & 15 } push @rows, [ @px[0 .. $ihdr{w} - 1] ] }
    }
    return { w => $ihdr{w}, h => $ihdr{h}, rows => \@rows, plte => $plte };
}
sub png_chunk { my ($type, $data) = @_; return pack('N', length $data) . $type . $data . pack('N', Compress::Zlib::crc32($type . $data)) }
sub png_write8
{
    my ($path, $w, $rows, $plte) = @_; # rows: array of arrays of bytes; plte: 256 [r,g,b]
    my $raw = join '', map { "\0" . pack('C*', @$_) } @$rows;
    spew($path, "\x89PNG\r\n\x1a\n"
         . png_chunk('IHDR', pack('N N C C C C C', $w, scalar(@$rows), 8, 3, 0, 0, 0))
         . png_chunk('PLTE', join('', map { pack('C3', @$_) } @$plte))
         . png_chunk('IDAT', Compress::Zlib::compress($raw, 9))
         . png_chunk('IEND', ''));
}
sub jasc_read
{
    my $path = shift;
    my @l = split /\r?\n/, slurp($path);
    die "$path: not a JASC palette\n" unless $l[0] eq 'JASC-PAL' && $l[1] eq '0100';
    return [ map { [ split / / ] } @l[3 .. 3 + $l[2] - 1] ];
}

# ---------------------------------------------------------------------------------------------
# Read pokefirered's picture
# ---------------------------------------------------------------------------------------------
my $gfx = png_read("$frlg/graphics/region_map/region_map.png");
die "region_map.png: expected $PNG_WIDTH px wide\n" unless $gfx->{w} == $PNG_WIDTH;
my $frPal = jasc_read("$frlg/graphics/region_map/region_map.pal");
my @frMap = unpack('v*', slurp("$frlg/graphics/region_map/kanto.bin"));
die "kanto.bin: expected $FR_MAP_W x $FR_MAP_H entries\n" unless scalar(@frMap) == $FR_MAP_W * $FR_MAP_H;
my $nFrTiles = $gfx->{h} / 8 * 16;

# the open-sea entry: the most frequent picture entry of the body
my (%count, %isPicture);
for my $fy ($FR_BODY_Y0 .. $FR_BODY_Y1)
{
    for my $fx ($FR_BODY_X0 .. $FR_BODY_X1)
    {
        my $e = $frMap[$fy * $FR_MAP_W + $fx];
        $count{$e}++ if (($e >> 12) & 0xF) < $FR_PICTURE_PALETTES;
    }
}
my ($seaEntry) = sort { $count{$b} <=> $count{$a} || $a <=> $b } keys %count;
die "kanto.bin: no picture entries in the body\n" unless defined $seaEntry;

# FRLG entry -> 8x8 block of FRLG (palette, colour) pairs
sub fr_block
{
    my $e = shift;
    my ($tile, $hf, $vf, $pal) = ($e & 0x3FF, ($e >> 10) & 1, ($e >> 11) & 1, ($e >> 12) & 0xF);
    die "kanto.bin: tile $tile is outside region_map.png ($nFrTiles tiles)\n" unless $tile < $nFrTiles;
    my @px;
    for my $y (0 .. 7)
    {
        my $sy = $vf ? 7 - $y : $y;
        for my $x (0 .. 7)
        {
            my $sx = $hf ? 7 - $x : $x;
            # PS-098: region_map.png is an 8-bit PNG whose pixel values already carry the palette number (16..31 on
            # the palette-1 tiles); gbagfx keeps only the low four bits when it builds the 4bpp tiles, and the
            # palette comes from the tilemap entry alone. Without the mask, pal * 16 + pixel landed in palette 2
            # (its 255,0,255 placeholders: the pink dots on every route and town cell of the PS-095 picture).
            push @px, [ $pal, $gfx->{rows}[int($tile / 16) * 8 + $sy][($tile % 16) * 8 + $sx] & 15 ];
        }
    }
    return \@px;
}

# ---------------------------------------------------------------------------------------------
# Build Emerald's tilemap, tiles and palette
# ---------------------------------------------------------------------------------------------
my (@tilemap, @tiles, %tileIdOf, @colours, %slotOf);
my $transparent = 0;
sub em_tile_of
{
    my $e = shift;
    return $tileIdOf{$e} if exists $tileIdOf{$e};
    my @px;
    for my $p (@{fr_block($e)})
    {
        my ($pal, $col) = @$p;
        if ($col == 0) { $transparent++; push @px, 0; next } # transparent in FRLG: transparent here too (none expected in the body)
        my $key = "$pal:$col";
        unless (exists $slotOf{$key})
        {
            $slotOf{$key} = scalar @colours;
            push @colours, $frPal->[$pal * 16 + $col];
        }
        push @px, $EM_PAL_BASE + $slotOf{$key};
    }
    my $id = scalar @tiles;
    push @tiles, \@px;
    $tileIdOf{$e} = $id;
    return $id;
}
em_tile_of($seaEntry); # tile 0 = open sea, like Hoenn's map.bin
for my $ty (0 .. $EM_TILEMAP_SIDE - 1)
{
    for my $tx (0 .. $EM_TILEMAP_SIDE - 1)
    {
        my ($fx, $fy) = ($tx - $OFF_X, $ty - $OFF_Y);
        my $e = $seaEntry;
        if ($fx >= $FR_BODY_X0 && $fx <= $FR_BODY_X1 && $fy >= $FR_BODY_Y0 && $fy <= $FR_BODY_Y1)
        {
            my $src = $frMap[$fy * $FR_MAP_W + $fx];
            $e = $src if (($src >> 12) & 0xF) < $FR_PICTURE_PALETTES; # frame and button tiles become sea
        }
        $tilemap[$ty * $EM_TILEMAP_SIDE + $tx] = em_tile_of($e);
    }
}
die sprintf("the picture needs %d tiles, Emerald's 8-bit tilemap allows %d\n", scalar @tiles, $EM_MAX_TILES) if @tiles > $EM_MAX_TILES;
die sprintf("the picture needs %d colours, the region map palette holds %d\n", scalar @colours, $EM_MAX_COLOURS) if @colours > $EM_MAX_COLOURS;
die "kanto.bin: $transparent transparent pixels inside the body (expected an opaque picture)\n" if $transparent;

# tiles.png: 16 tiles per row, the last row padded with sea
my $nRows = int((scalar(@tiles) + 15) / 16);
my @pngRows = map { [ (0) x $PNG_WIDTH ] } 1 .. $nRows * 8;
for my $i (0 .. $nRows * 16 - 1)
{
    my $px = $i < @tiles ? $tiles[$i] : $tiles[0];
    for my $y (0 .. 7) { for my $x (0 .. 7) { $pngRows[int($i / 16) * 8 + $y][($i % 16) * 8 + $x] = $px->[$y * 8 + $x] } }
}
my @plte = map { [0, 0, 0] } 0 .. 255;
$plte[$EM_PAL_BASE + $_] = $colours[$_] for 0 .. $#colours;
png_write8("$outDir/kanto_map.png", $PNG_WIDTH, \@pngRows, \@plte);
spew("$outDir/kanto_map.pal", "JASC-PAL\n0100\n$EM_MAX_COLOURS\n" . join('', map { my $c = $_ < @colours ? $colours[$_] : [0, 0, 0]; "@$c\n" } 0 .. $EM_MAX_COLOURS - 1));
spew("$outDir/kanto_map.bin", pack('C*', @tilemap));
printf "kanto_region_map: %d tiles (%d rows of 16 in kanto_map.png), %d colours at 0x%02X.., picture body at tiles %d..%d x %d..%d, cursor cell (gx,gy) on tile (gx+%d, gy+%d)\n",
    scalar @tiles, $nRows, scalar @colours, $EM_PAL_BASE, $FR_BODY_X0 + $OFF_X, $FR_BODY_X1 + $OFF_X, $FR_BODY_Y0 + $OFF_Y, $FR_BODY_Y1 + $OFF_Y, $EM_CURSOR_X_MIN, $EM_CURSOR_Y_MIN;

# ---------------------------------------------------------------------------------------------
# Optional preview: the written Emerald data rendered x3 with the cursor grid and the Kanto map
# sections of src/data/region_map/region_map_sections.json (towns as filled squares, routes as dots)
# ---------------------------------------------------------------------------------------------
if (defined $preview)
{
    my $S = 3;
    my ($VW, $VH) = (30, 20); # the visible screen in tiles
    my @img = map { [ (0) x ($VW * 8 * $S) ] } 1 .. $VH * 8 * $S;
    for my $ty (0 .. $VH - 1)
    {
        for my $tx (0 .. $VW - 1)
        {
            my $px = $tiles[$tilemap[$ty * $EM_TILEMAP_SIDE + $tx]];
            for my $y (0 .. 7) { for my $x (0 .. 7) {
                my $v = $px->[$y * 8 + $x];
                for my $dy (0 .. $S - 1) { for my $dx (0 .. $S - 1) { $img[($ty * 8 + $y) * $S + $dy][($tx * 8 + $x) * $S + $dx] = $v } }
            } }
        }
    }
    my ($GRID, $TOWN, $ROUTE) = (0xFE, 0xFD, 0xFC);
    $plte[$GRID] = [255, 255, 255]; $plte[$TOWN] = [255, 0, 0]; $plte[$ROUTE] = [255, 0, 255];
    for my $gx (0 .. 28) { my $x = ($gx + $EM_CURSOR_X_MIN) * 8 * $S; next if $x >= $VW * 8 * $S; for my $y ($EM_CURSOR_Y_MIN * 8 * $S .. ($EM_CURSOR_Y_MIN + 15) * 8 * $S - 1) { $img[$y][$x] = $GRID if $y < $VH * 8 * $S } }
    for my $gy (0 .. 15) { my $y = ($gy + $EM_CURSOR_Y_MIN) * 8 * $S; next if $y >= $VH * 8 * $S; for my $x ($EM_CURSOR_X_MIN * 8 * $S .. ($EM_CURSOR_X_MIN + 28) * 8 * $S - 1) { $img[$y][$x] = $GRID if $x < $VW * 8 * $S } }
    my $sections = "$repo/src/data/region_map/region_map_sections.json";
    if (-f $sections)
    {
        my $text = slurp($sections);
        while ($text =~ /\{\s*"id":\s*"(MAPSEC_KANTO_\w+)",\s*"comment":[^}]*?"name":\s*"([^"]*)",\s*"x":\s*(\d+),\s*"y":\s*(\d+),\s*"width":\s*(\d+),\s*"height":\s*(\d+)/g)
        {
            my ($id, $name, $x, $y, $w, $h) = ($1, $2, $3, $4, $5, $6);
            next if $id =~ /_MAP$/ || $name !~ /^(ROUTE \d+|.* (TOWN|CITY|ISLAND|PLATEAU))$/; # outdoor sections only
            my $town = $name !~ /^ROUTE/;
            for my $dy (0 .. $h - 1) { for my $dx (0 .. $w - 1) {
                my ($cx, $cy) = (($x + $dx + $EM_CURSOR_X_MIN) * 8 * $S + 4 * $S, ($y + $dy + $EM_CURSOR_Y_MIN) * 8 * $S + 4 * $S);
                my $r = $town ? 2 * $S : $S - 1;
                for my $yy ($cy - $r .. $cy + $r) { for my $xx ($cx - $r .. $cx + $r) { $img[$yy][$xx] = $town ? $TOWN : $ROUTE if $yy >= 0 && $yy < $VH * 8 * $S && $xx >= 0 && $xx < $VW * 8 * $S } }
            } }
        }
    }
    png_write8($preview, $VW * 8 * $S, \@img, \@plte);
    print "preview written to $preview\n";
}
exit 0;
