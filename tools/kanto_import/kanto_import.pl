#!/usr/bin/perl
# tools/kanto_import/kanto_import.pl -- PS-093/PS-094/PS-095: import Kanto map data from a pret pokefirered
# checkout into this repository as REGION_KANTO (maps, layouts, tilesets, sections, heal
# locations, visited flags, map groups, region map layout/Fly spots, mp_presence group table).
#
#   perl tools/kanto_import/kanto_import.pl --frlg <pokefirered checkout> --maps <list file> [--repo <root>]
#                                           [--allow-unknown] [--dry-run] [--quiet]
#
# Perl core modules only (JSON::PP, Compress::Zlib, File::*). See README.md in this directory for
# what it writes, the one-time hooks it expects in the repository, and the tileset re-pack
# ("prefix re-pack", card PS-094: FRLG primaries hold 640 tiles / 640 metatiles / 7 palettes,
# Emerald's engine splits at 512 / 512 / 6; the tail of a FRLG primary is prepended to every
# secondary paired with it, so no metatile or tile id changes). PS-095 added: one re-packed copy
# per primary for a secondary used with several primaries, shared tileset graphics (silph_co uses
# condominiums' tiles), map coordinates for dungeon / indoor sections, the Porymap marker blocks in
# src/data/tilesets/*.h, every heal location, hand-written extras (tools/kanto_import/extras) and
# the audits printed at the end (warps, connections, layouts, music, behaviours).
# The Kanto picture is a separate step: tools/kanto_import/kanto_region_map.pl.
#
# The pokefirered files are data: nothing is executed from that checkout.
use strict;
use warnings;
use JSON::PP;
use Compress::Zlib ();
use File::Basename qw(dirname basename);
use File::Path qw(make_path remove_tree);
use File::Spec;
use Getopt::Long;

# ---------------------------------------------------------------------------------------------
# Constants of the two engines
# ---------------------------------------------------------------------------------------------
my $FR_TILES_IN_PRIMARY = 640;      # pokefirered include/fieldmap.h
my $FR_METATILES_IN_PRIMARY = 640;
my $FR_PALS_IN_PRIMARY = 7;
my $EM_TILES_IN_PRIMARY = 512;      # ours, include/fieldmap.h
my $EM_METATILES_IN_PRIMARY = 512;
my $EM_PALS_IN_PRIMARY = 6;
my $TILES_TOTAL = 1024;             # both
my $METATILES_TOTAL = 1024;         # both
my $PALS_TOTAL = 16;                # palette files per tileset
my $PREFIX_TILES = $FR_TILES_IN_PRIMARY - $EM_TILES_IN_PRIMARY;         # 128
my $PREFIX_METATILES = $FR_METATILES_IN_PRIMARY - $EM_METATILES_IN_PRIMARY; # 128
my $MAX_SECONDARY_TILES = $TILES_TOTAL - $FR_TILES_IN_PRIMARY;          # 384
my $MAX_SECONDARY_METATILES = $METATILES_TOTAL - $FR_METATILES_IN_PRIMARY; # 384
my $TILE_PX = 8;
my $PNG_WIDTH = 128;                # every tileset png is 16 tiles wide
my $REGION_MAP_WIDTH = 28;          # include/region.h
my $REGION_MAP_HEIGHT = 15;
my $FIRST_REGION_MAPSEC = '0x100';  # MAPSEC_REGIONS_START: Kanto is the first region after Hoenn
my $FIRST_FLAG_OFFSET = 0x10;       # REGION_FLAGS_START + 0x00..0x0F are FLAG_BADGE09..24

my $PREFIX = 'Kanto';               # map / layout / tileset name prefix
my $UPREFIX = 'KANTO';              # constant prefix
my $CARDS = 'PS-094/PS-095';

# ---------------------------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------------------------
my ($frlg, $mapsFile, $repo, $allowUnknown, $dryRun, $quiet);
GetOptions('frlg=s' => \$frlg, 'maps=s' => \$mapsFile, 'repo=s' => \$repo,
           'allow-unknown' => \$allowUnknown, 'dry-run' => \$dryRun, 'quiet' => \$quiet)
    or die "usage: $0 --frlg <pokefirered> --maps <list> [--repo <root>] [--allow-unknown] [--dry-run]\n";
die "--frlg <pokefirered checkout> is required\n" unless defined $frlg && -d "$frlg/data/maps";
die "--maps <list file> is required\n" unless defined $mapsFile && -f $mapsFile;
$repo //= File::Spec->rel2abs(File::Spec->catdir(dirname(__FILE__), '..', '..'));
die "repo root $repo has no data/maps\n" unless -d "$repo/data/maps";
my $toolDir = dirname(__FILE__);
my $extrasDir = "$toolDir/extras";

my (@errors, @notes, @audit);
sub err  { push @errors, shift }
sub note { push @notes, shift }
sub audit { push @audit, shift }
sub info { print @_, "\n" unless $quiet }

# ---------------------------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------------------------
sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; return $d }
sub rjson { return JSON::PP->new->decode(slurp(shift)) } # bytes in, bytes out: non-ASCII names (POKéMON) pass through unchanged
my %pendingWrites; # path => content, written at the end (phase 2)
sub queue_write { my ($path, $content) = @_; $pendingWrites{$path} = $content }
sub flush_writes
{
    for my $path (sort keys %pendingWrites)
    {
        make_path(dirname($path));
        open my $h, '>', $path or die "cannot write $path: $!\n";
        binmode $h;
        print $h $pendingWrites{$path};
        close $h;
    }
}
sub camel { my $s = shift; return join '', map { ucfirst } split /_/, $s }
sub upper_snake_from_camel { my $s = shift; $s =~ s/([a-z0-9])([A-Z])/$1_$2/g; return uc $s }

# ---------------------------------------------------------------------------------------------
# Translation tables
# ---------------------------------------------------------------------------------------------
sub read_tsv
{
    my $f = shift; my %t;
    for (split /\n/, slurp($f))
    {
        s/\r$//; # CRLF checkouts
        next if /^\s*(#|$)/;
        my ($from, $to, $noteText) = split /\t/;
        $t{$from} = { to => $to, note => $noteText // '' };
    }
    return \%t;
}
my $behaviorTable = read_tsv("$toolDir/behaviors.tsv");
my $constTable = read_tsv("$toolDir/constants.tsv");

# FRLG behaviour ids -> names (#define MB_X 0xNN)
my %frBehaviorName;
for (split /\n/, slurp("$frlg/include/constants/metatile_behaviors.h"))
{
    $frBehaviorName{hex $2} = $1 if /^#define (MB_\w+)\s+(0x[0-9A-Fa-f]+)/;
}
# our behaviour names -> ids (enum)
my %ourBehaviorId;
{
    my $i = 0;
    for (split /\n/, slurp("$repo/include/constants/metatile_behaviors.h"))
    {
        if (/^\s+(MB_\w+),/) { $ourBehaviorId{$1} = $i++ }
    }
}
die "could not parse our metatile_behaviors.h\n" unless exists $ourBehaviorId{MB_NORMAL} && exists $ourBehaviorId{MB_ANIMATED_DOOR};
for my $from (keys %$behaviorTable)
{
    my $to = $behaviorTable->{$from}{to};
    die "behaviors.tsv: $from -> $to is not an Emerald behaviour\n" unless $to eq '-' || exists $ourBehaviorId{$to};
}

my $ourSongs = slurp("$repo/include/constants/songs.h");
my $ourWeather = slurp("$repo/include/constants/weather.h");
my $ourMapTypes = slurp("$repo/include/constants/map_types.h");
sub our_has_define { my ($text, $name) = @_; return $text =~ /^#define \Q$name\E\b/m }

my %behaviorUnknownRefs; # "tileset:metatile" => frlg name, only for referenced metatiles
my %behaviorUnknownUnref;
my %behaviorUsed;        # frlg name => count (referenced cells)
sub translate_behavior
{
    my ($frId) = @_;
    my $frName = $frBehaviorName{$frId} // sprintf("MB_UNKNOWN_%02X", $frId);
    my $to;
    if (exists $behaviorTable->{$frName})       { $to = $behaviorTable->{$frName}{to} }
    elsif (exists $ourBehaviorId{$frName})      { $to = $frName }
    else                                        { $to = '-' }
    return ($frName, $to);
}

my %musicUsed; # our track => count
sub translate_music
{
    my $mus = shift;
    (my $rg = $mus) =~ s/^MUS_/MUS_RG_/;
    my $to;
    if    (our_has_define($ourSongs, $rg)) { $to = $rg }
    elsif (our_has_define($ourSongs, $mus)) { $to = $mus }
    elsif (exists $constTable->{$mus} && our_has_define($ourSongs, $constTable->{$mus}{to})) { $to = $constTable->{$mus}{to} }
    else { err("music $mus has no Emerald track (add a row to constants.tsv)"); $to = 'MUS_NONE' }
    $musicUsed{$to}++;
    return $to;
}
sub translate_const
{
    my ($name, $file, $what) = @_;
    return $name if our_has_define($file, $name);
    return $constTable->{$name}{to} if exists $constTable->{$name} && our_has_define($file, $constTable->{$name}{to});
    err("$what $name has no Emerald constant (add a row to constants.tsv)");
    return $name;
}

# ---------------------------------------------------------------------------------------------
# PNG (4bpp indexed, 128 px wide, not interlaced: every pokefirered tileset)
# ---------------------------------------------------------------------------------------------
sub png_read
{
    my $path = shift;
    my $d = slurp($path);
    die "$path: not a PNG\n" unless substr($d, 0, 8) eq "\x89PNG\r\n\x1a\n";
    my $pos = 8;
    my (%ihdr, $plte);
    my $idat = "";
    while ($pos + 8 <= length $d)
    {
        my ($len, $type) = unpack('N a4', substr($d, $pos, 8));
        my $data = substr($d, $pos + 8, $len);
        if ($type eq 'IHDR')
        {
            @ihdr{qw(w h bd ct cm fl il)} = unpack('N N C C C C C', $data);
        }
        elsif ($type eq 'PLTE') { $plte = $data }
        elsif ($type eq 'IDAT') { $idat .= $data }
        $pos += 12 + $len;
        last if $type eq 'IEND';
    }
    die "$path: expected a 4bpp indexed, non-interlaced PNG $PNG_WIDTH px wide (got w=$ihdr{w} bd=$ihdr{bd} ct=$ihdr{ct} il=$ihdr{il})\n"
        unless $ihdr{w} == $PNG_WIDTH && $ihdr{bd} == 4 && $ihdr{ct} == 3 && $ihdr{il} == 0;
    my $raw = Compress::Zlib::uncompress($idat);
    die "$path: zlib inflate failed\n" unless defined $raw;
    my $stride = $ihdr{w} / 2; # 4 bits per pixel
    die "$path: IDAT size mismatch\n" unless length($raw) == $ihdr{h} * ($stride + 1);
    my @rows;
    my @prev = (0) x $stride;
    for my $y (0 .. $ihdr{h} - 1)
    {
        my $filter = ord(substr($raw, $y * ($stride + 1), 1));
        my @x = unpack('C*', substr($raw, $y * ($stride + 1) + 1, $stride));
        for my $i (0 .. $stride - 1)
        {
            my $a = $i > 0 ? $x[$i - 1] : 0;   # bytes per pixel rounds up to 1 for 4bpp
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
        push @rows, pack('C*', @x);
        @prev = @x;
    }
    return { w => $ihdr{w}, h => $ihdr{h}, plte => $plte, rows => \@rows };
}

sub png_chunk { my ($type, $data) = @_; return pack('N', length $data) . $type . $data . pack('N', Compress::Zlib::crc32($type . $data)) }
sub png_build
{
    my ($w, $plte, $rows) = @_;
    my $h = scalar @$rows;
    my $raw = join '', map { "\0" . $_ } @$rows;
    return "\x89PNG\r\n\x1a\n"
         . png_chunk('IHDR', pack('N N C C C C C', $w, $h, 4, 3, 0, 0, 0))
         . png_chunk('PLTE', $plte)
         . png_chunk('IDAT', Compress::Zlib::compress($raw, 9))
         . png_chunk('IEND', '');
}

# ---------------------------------------------------------------------------------------------
# pokefirered data
# ---------------------------------------------------------------------------------------------
# tileset symbol -> directory
my %frTileset; # gTileset_X => { dir, base, primary, gfxOwner }
for my $kind (qw(primary secondary))
{
    for my $d (glob("$frlg/data/tilesets/$kind/*"))
    {
        next unless -d $d;
        my $base = basename($d);
        my $sym = $base eq 'ss_anne' ? 'SSAnne' : camel($base);
        $frTileset{"gTileset_$sym"} = { dir => $d, base => $base, primary => ($kind eq 'primary') };
    }
}
{
    # pokefirered's headers.h says which tileset's tiles / palettes a tileset draws with (silph_co uses
    # condominiums' graphics and only has metatiles of its own)
    my $hdr = slurp("$frlg/src/data/tilesets/headers.h");
    while ($hdr =~ /const struct Tileset (gTileset_\w+) =\s*\{(.*?)\};/sg)
    {
        my ($sym, $body) = ($1, $2);
        unless ($frTileset{$sym}) { note("pokefirered tileset $sym has no data/tilesets directory with that name (fine unless a chosen map uses it)"); next }
        (my $own = $sym) =~ s/^gTileset_//;
        my ($tiles) = $body =~ /\.tiles\s*=\s*gTilesetTiles_(\w+)/;
        my ($pals) = $body =~ /\.palettes\s*=\s*gTilesetPalettes_(\w+)/;
        next unless defined $tiles && defined $pals;
        next if $tiles eq $own && $pals eq $own;
        if ($tiles eq $pals && $frTileset{"gTileset_$tiles"}) { $frTileset{$sym}{gfxOwner} = "gTileset_$tiles" }
        else { err("tileset $sym: draws with gTilesetTiles_$tiles / gTilesetPalettes_$pals, which is not one tileset's graphics; not supported") }
    }
}
sub fr_tileset
{
    my $sym = shift;
    my $t = $frTileset{$sym} or die "unknown pokefirered tileset $sym\n";
    return $t if $t->{loaded};
    $t->{loaded} = 1;
    my $gfxDir = $t->{gfxOwner} ? $frTileset{$t->{gfxOwner}}{dir} : $t->{dir};
    for my $f (["$t->{dir}/metatiles.bin"], ["$t->{dir}/metatile_attributes.bin"], ["$gfxDir/tiles.png"])
    {
        unless (-f $f->[0]) { err("tileset $sym: $f->[0] is missing"); $t->{broken} = 1 }
    }
    unless (-d "$gfxDir/palettes") { err("tileset $sym: no palettes directory in $gfxDir"); $t->{broken} = 1 }
    return $t if $t->{broken};
    $t->{metatiles} = [ unpack('v*', slurp("$t->{dir}/metatiles.bin")) ];
    $t->{attrs} = [ unpack('V*', slurp("$t->{dir}/metatile_attributes.bin")) ];
    $t->{nMetatiles} = scalar(@{$t->{attrs}});
    die "tileset $sym: metatiles.bin and metatile_attributes.bin disagree\n" unless scalar(@{$t->{metatiles}}) == 8 * $t->{nMetatiles};
    $t->{png} = png_read("$gfxDir/tiles.png");
    for my $i (0 .. $PALS_TOTAL - 1)
    {
        my $p = sprintf("%s/palettes/%02d.pal", $gfxDir, $i);
        if (-f $p) { $t->{pal}[$i] = slurp($p) } else { err("tileset $sym: palette file $p is missing") }
    }
    return $t;
}

my $frLayouts = rjson("$frlg/data/layouts/layouts.json");
my %frLayoutById = map { $_->{id} => $_ } grep { defined $_->{id} } @{$frLayouts->{layouts}};
my $frGroups = rjson("$frlg/data/maps/map_groups.json");
my $frSections = rjson("$frlg/src/data/region_map/region_map_sections.json");
my %frSectionById = map { $_->{id} => $_ } @{$frSections->{map_sections}};
my $frHeal = rjson("$frlg/src/data/heal_locations.json");
my %frHealByMap; # MAP_X => entry (the first one)
for my $h (@{$frHeal->{heal_locations}}) { $frHealByMap{$h->{map}} //= $h }
# pokefirered's region map layers: which sections sit on the cursor grid (LAYER_MAP) and where its
# dungeon icons are (LAYER_DUNGEON); region_map_sections.json carries x=0,y=0 for the dungeons
my (%frOnMapLayer, %frDungeonCell);
{
    my $text = slurp("$frlg/src/data/region_map/region_map_layout_kanto.h");
    my ($mapLayer, $dungeonLayer) = $text =~ /\[LAYER_MAP\]\s*=\s*\{(.*?)\[LAYER_DUNGEON\]\s*=\s*\{(.*)/s;
    die "could not parse pokefirered's region_map_layout_kanto.h\n" unless defined $dungeonLayer;
    $frOnMapLayer{$_} = 1 for $mapLayer =~ /(MAPSEC_\w+)/g;
    my $y = 0;
    while ($dungeonLayer =~ /\{([^{}]*)\}/g)
    {
        my @cells = map { s/^\s+|\s+$//gr } split /,/, $1;
        for my $x (0 .. $#cells) { $frDungeonCell{$cells[$x]} //= [$x, $y] if $cells[$x] =~ /^MAPSEC_/ && $cells[$x] ne 'MAPSEC_NONE' }
        $y++;
    }
}
# every pokefirered map id (to tell a warp to a map outside the import from a typo)
my %frAllMapIds; # MAP_X => map name
for my $g (@{$frGroups->{group_order}}) { for my $m (@{$frGroups->{$g}}) { my $j = rjson("$frlg/data/maps/$m/map.json"); $frAllMapIds{$j->{id}} = $m } }

# map list
my @mapNames;
for (split /\n/, slurp($mapsFile))
{
    s/#.*//; s/^\s+|\s+$//g;
    next unless length;
    push @mapNames, $_;
}
die "no maps listed in $mapsFile\n" unless @mapNames;
my %chosen = map { $_ => 1 } @mapNames;

# ---------------------------------------------------------------------------------------------
# Naming
# ---------------------------------------------------------------------------------------------
sub our_map_name   { return $PREFIX . shift }                                   # PalletTown -> KantoPalletTown
sub our_map_id     { my $id = shift; $id =~ s/^MAP_/MAP_${UPREFIX}_/; return $id }
sub our_layout_id  { my $id = shift; $id =~ s/^LAYOUT_/LAYOUT_${UPREFIX}_/; return $id }
sub our_layout_dir { my $name = shift; $name =~ s/_Layout$//; return $PREFIX . $name }   # PalletTown_Layout -> KantoPalletTown
# A secondary used with several primaries gets one re-packed copy per primary, named after the
# primary (kanto_seafoam_islands_general / gTileset_KantoSeafoamIslandsGeneral); with one primary the
# plain name is kept.
my %tilesetPairs;  # secondary sym => { primary sym => 1 }
sub tileset_suffix
{
    my ($sym, $psym) = @_;
    return '' if $frTileset{$sym}{primary} || !defined $psym || scalar(keys %{$tilesetPairs{$sym}}) < 2;
    (my $p = $psym) =~ s/^gTileset_//;
    return $p;
}
sub our_tileset_sym{ my ($sym, $psym) = @_; (my $s = $sym) =~ s/^gTileset_//; return "gTileset_$PREFIX$s" . tileset_suffix($sym, $psym) }
sub our_tileset_var{ my ($sym, $psym, $what) = @_; (my $s = $sym) =~ s/^gTileset_//; return "$what\_$PREFIX$s" . tileset_suffix($sym, $psym) } # gTilesetTiles_KantoGeneral
sub our_tileset_dir{ my ($sym, $psym) = @_; my $t = $frTileset{$sym}; my $sfx = tileset_suffix($sym, $psym); $sfx = '_' . lc($frTileset{$psym}{base}) if $sfx ne ''; return ($t->{primary} ? 'primary' : 'secondary') . '/' . lc($PREFIX) . "_$t->{base}$sfx" }
# Emerald already defines a few MAPSEC_KANTO_* (met-location names for Pokémon traded from FRLG, e.g.
# MAPSEC_KANTO_VICTORY_ROAD, in Hoenn's id range); a Kanto map section that would collide gets "_MAP".
my $GENERATED_MARK = 'kanto_import';
my %existingSectionIds;
{
    my $text = slurp("$repo/src/data/region_map/region_map_sections.json");
    my ($open, $close) = json_array_span($text, 'map_sections');
    for my $el (json_array_elements(substr($text, $open + 1, $close - $open - 1)))
    {
        next if $el =~ /"comment": "\Q$GENERATED_MARK\E/;
        $existingSectionIds{$1} = 1 if $el =~ /"id": "(MAPSEC_\w+)"/;
    }
}
my %sectionRenamed;
sub our_section_id
{
    my $id = shift; $id =~ s/^MAPSEC_/MAPSEC_${UPREFIX}_/;
    if ($existingSectionIds{$id})
    {
        note("section $id already exists in Emerald (met-location name); Kanto's own section is ${id}_MAP") unless $sectionRenamed{$id}++;
        $id .= '_MAP';
    }
    return $id;
}
sub our_heal_id    { my $id = shift; $id =~ s/^HEAL_LOCATION_/HEAL_LOCATION_${UPREFIX}_/; return $id }
sub our_flag_id    { my $sec = shift; $sec =~ s/^MAPSEC_${UPREFIX}_//; return "FLAG_VISITED_${UPREFIX}_$sec" }
sub our_group_name { my $g = shift; $g =~ s/^gMapGroup_/gMapGroup_$PREFIX/; return $g }

# ---------------------------------------------------------------------------------------------
# Phase 1: read the chosen maps, decide what is generated, collect errors
# ---------------------------------------------------------------------------------------------
my (%map, @mapOrder, %groupMaps, @groupOrder);
for my $g (@{$frGroups->{group_order}})
{
    for my $m (@{$frGroups->{$g}})
    {
        next unless $chosen{$m};
        my $j = rjson("$frlg/data/maps/$m/map.json");
        $map{$m} = { fr => $j, group => $g };
        push @mapOrder, $m;
        push @groupOrder, $g unless grep { $_ eq $g } @groupOrder;
        push @{$groupMaps{$g}}, $m;
        delete $chosen{$m};
    }
}
err("map $_ is not in pokefirered's map_groups.json") for sort keys %chosen;
my %frIdToName = map { $map{$_}{fr}{id} => $_ } keys %map;

# layouts and tilesets used
my (%layoutUsed, %tilesetUsed);
for my $m (@mapOrder)
{
    my $L = $frLayoutById{$map{$m}{fr}{layout}} or do { err("map $m: layout $map{$m}{fr}{layout} not in layouts.json"); next };
    $layoutUsed{$L->{id}} = $L;
}
for my $L (values %layoutUsed)
{
    my ($p, $s) = ($L->{primary_tileset}, $L->{secondary_tileset});
    for ($p, $s) { err("layout $L->{id}: tileset $_ unknown") unless $frTileset{$_ // ''} }
    next unless $frTileset{$p // ''} && $frTileset{$s // ''};
    err("layout $L->{id}: $p is not a primary tileset") unless $frTileset{$p}{primary};
    err("layout $L->{id}: $s is not a secondary tileset") if $frTileset{$s}{primary};
    $tilesetUsed{$p} = 1; $tilesetUsed{$s} = 1;
    $tilesetPairs{$s}{$p} = 1;
}
for my $s (sort keys %tilesetPairs)
{
    my @p = sort keys %{$tilesetPairs{$s}};
    note("secondary tileset $s is used with several primaries (@p): one re-packed copy per primary") if @p > 1;
}
# hand-written extras (tools/kanto_import/extras/<Map>.objects.json / <Map>.scripts.inc)
my %extras;
if (-d $extrasDir)
{
    for my $f (sort glob("$extrasDir/*"))
    {
        my $base = basename($f);
        my ($m, $kind) = $base =~ /^(\w+)\.(objects\.json|scripts\.inc)$/ or do { note("extras: $base ignored (expected <Map>.objects.json or <Map>.scripts.inc)"); next };
        unless ($map{$m}) { err("extras: $base belongs to map $m, which is not in the map list"); next }
        $extras{$m}{$kind} = slurp($f);
        $extras{$m}{$kind} =~ s/\r\n/\n/g;
    }
}
exit_if_errors('reading the map list');

# load tilesets
fr_tileset($_) for sort keys %tilesetUsed;
exit_if_errors('loading tilesets');

# metatile ids referenced by the chosen layouts, per tileset symbol and (FRLG) metatile id
my %referenced; # sym => { frMetatileId => 1 }
my $cellsChecked = 0;
for my $L (sort { $a->{id} cmp $b->{id} } values %layoutUsed)
{
    my @body = unpack('v*', slurp("$frlg/$L->{blockdata_filepath}"));
    my @borderAll = unpack('v*', slurp("$frlg/$L->{border_filepath}"));
    my $nCells = $L->{width} * $L->{height};
    err("layout $L->{id}: map.bin has " . scalar(@body) . " cells, expected $nCells") unless scalar(@body) == $nCells;
    # pokefirered tiles borders of any size (border_width / border_height); Emerald's engine tiles a
    # 2x2 border, so the top-left 2x2 of a bigger one is kept
    my ($bw, $bh) = ($L->{border_width} // 2, $L->{border_height} // 2);
    err("layout $L->{id}: border.bin has " . scalar(@borderAll) . " cells, expected ${bw}x$bh") unless scalar(@borderAll) == $bw * $bh;
    err("layout $L->{id}: border ${bw}x$bh is smaller than Emerald's 2x2") if $bw < 2 || $bh < 2;
    next if $bw < 2 || $bh < 2 || scalar(@borderAll) != $bw * $bh;
    note("layout $L->{id}: pokefirered border is ${bw}x$bh, Emerald tiles a 2x2 border: its top-left 2x2 is used (the pattern outside the map edge differs from FRLG)") if $bw != 2 || $bh != 2;
    my @border = ($borderAll[0], $borderAll[1], $borderAll[$bw], $borderAll[$bw + 1]);
    my @cells = (@body, @border);
    my $s = fr_tileset($L->{secondary_tileset});
    my $secondaryCount = $s->{nMetatiles} > $MAX_SECONDARY_METATILES ? $MAX_SECONDARY_METATILES : $s->{nMetatiles};
    for my $c (@cells)
    {
        my $id = $c & 0x3FF;
        $cellsChecked++;
        if ($id < $FR_METATILES_IN_PRIMARY) { $referenced{$L->{primary_tileset}}{$id} = 1 }
        elsif ($id - $FR_METATILES_IN_PRIMARY < $secondaryCount) { $referenced{$L->{secondary_tileset}}{$id} = 1 }
        else { err("layout $L->{id}: metatile id $id is outside $L->{secondary_tileset} ($secondaryCount metatiles)") }
    }
    $L->{cells} = \@cells;
}

# behaviour translation of one tileset's attribute list -> 16-bit attributes
sub convert_attributes
{
    my ($sym, $t, $from, $to, $idBase) = @_; # metatile indices [$from, $to) of the FRLG file; FRLG id = $idBase + index
    my @out;
    for my $i ($from .. $to - 1)
    {
        my $attr = $t->{attrs}[$i] // 0;
        my $frBeh = $attr & 0x1FF;
        my $layer = ($attr >> 29) & 3;
        my ($frName, $toName) = translate_behavior($frBeh);
        my $frId = $idBase + $i;
        if ($toName eq '-')
        {
            if ($referenced{$sym}{$frId}) { $behaviorUnknownRefs{"$sym metatile $frId"} = $frName }
            else                          { $behaviorUnknownUnref{$frName}++ }
            $toName = 'MB_NORMAL';
        }
        $behaviorUsed{$frName}++ if $referenced{$sym}{$frId};
        my $other = $attr & ~(0x1FF | 0x3E00 | 0x07000000 | 0x60000000);
        note(sprintf("tileset %s metatile %d: attribute bits 0x%08X outside behaviour/terrain/encounter/layer were dropped", $sym, $frId, $other)) if $other;
        push @out, ($ourBehaviorId{$toName} & 0xFF) | ($layer << 12);
    }
    return @out;
}

# tilesets -> files. Primaries first, then every (secondary, primary) pair.
my (@gfxLines, @metaLines, @hdrLines);
my %writtenTilesets;
my @tilesetOutputs = map { [$_, undef] } sort grep { $frTileset{$_}{primary} } keys %tilesetUsed;
for my $s (sort grep { !$frTileset{$_}{primary} } keys %tilesetUsed) { push @tilesetOutputs, map { [$s, $_] } sort keys %{$tilesetPairs{$s}} }
my %attrsConverted; # "$sym|$psym" => 1: convert_attributes is counted once per output copy
for my $out (@tilesetOutputs)
{
    my ($sym, $psym) = @$out;
    my $t = fr_tileset($sym);
    my $oursym = our_tileset_sym($sym, $psym);
    my $dir = our_tileset_dir($sym, $psym);
    my $relDir = "data/tilesets/$dir";
    my (@rows, @metatiles, @attrs, @pal);
    my $nTiles;
    if ($t->{primary})
    {
        die "primary $sym: expected $FR_TILES_IN_PRIMARY tiles / $FR_METATILES_IN_PRIMARY metatiles\n"
            unless $t->{png}{h} == $FR_TILES_IN_PRIMARY / 16 * $TILE_PX && $t->{nMetatiles} == $FR_METATILES_IN_PRIMARY;
        @rows = @{$t->{png}{rows}}[0 .. $EM_TILES_IN_PRIMARY / 16 * $TILE_PX - 1];
        @metatiles = @{$t->{metatiles}}[0 .. 8 * $EM_METATILES_IN_PRIMARY - 1];
        @attrs = convert_attributes($sym, $t, 0, $EM_METATILES_IN_PRIMARY, 0);
        @pal = @{$t->{pal}};
        $nTiles = $EM_TILES_IN_PRIMARY;
    }
    else
    {
        my $p = fr_tileset($psym);
        my $secRows = $t->{png}{h};
        my $maxRows = $MAX_SECONDARY_TILES / 16 * $TILE_PX;
        if ($secRows > $maxRows) { note("tileset $sym: tiles.png has " . ($secRows / $TILE_PX * 16) . " tiles, only $MAX_SECONDARY_TILES are addressable; the rest were dropped"); $secRows = $maxRows }
        @rows = (@{$p->{png}{rows}}[$EM_TILES_IN_PRIMARY / 16 * $TILE_PX .. $FR_TILES_IN_PRIMARY / 16 * $TILE_PX - 1], @{$t->{png}{rows}}[0 .. $secRows - 1]);
        my $secMeta = $t->{nMetatiles};
        if ($secMeta > $MAX_SECONDARY_METATILES) { note("tileset $sym: $secMeta metatiles, only $MAX_SECONDARY_METATILES are addressable; the rest were dropped"); $secMeta = $MAX_SECONDARY_METATILES }
        @metatiles = (@{$p->{metatiles}}[8 * $EM_METATILES_IN_PRIMARY .. 8 * $FR_METATILES_IN_PRIMARY - 1], @{$t->{metatiles}}[0 .. 8 * $secMeta - 1]);
        @attrs = (convert_attributes($psym, $p, $EM_METATILES_IN_PRIMARY, $FR_METATILES_IN_PRIMARY, 0),
                  convert_attributes($sym, $t, 0, $secMeta, $FR_METATILES_IN_PRIMARY));
        @pal = @{$t->{pal}};
        $pal[$EM_PALS_IN_PRIMARY] = $p->{pal}[$EM_PALS_IN_PRIMARY]; # FRLG primary palette 6 lives in Emerald's first secondary slot
        $nTiles = $PREFIX_TILES + $secRows / $TILE_PX * 16;
    }
    # shared graphics: when the tileset draws with another imported tileset's tiles and palettes and
    # both pair with the same primary, the re-packed graphics are identical, so the symbols are shared
    my $owner = $t->{gfxOwner};
    my $shareWith = (defined $owner && $tilesetUsed{$owner} && (!defined $psym || $tilesetPairs{$owner}{$psym})) ? $owner : undef;
    my $gfxSym = $shareWith // $sym;
    my $tilesVar = our_tileset_var($gfxSym, $psym, 'gTilesetTiles');
    my $palVar = our_tileset_var($gfxSym, $psym, 'gTilesetPalettes');
    my $metaVar = our_tileset_var($sym, $psym, 'gMetatiles');
    my $attrVar = our_tileset_var($sym, $psym, 'gMetatileAttributes');
    if (defined $shareWith)
    {
        note("tileset $sym: shares " . our_tileset_sym($shareWith, $psym) . "'s tiles and palettes (as in pokefirered)");
    }
    else
    {
        note("tileset $sym: pokefirered draws it with $owner\'s graphics, which is not imported with the same primary; it gets its own copy") if defined $owner;
        queue_write("$repo/$relDir/tiles.png", png_build($PNG_WIDTH, $t->{png}{plte}, \@rows));
        queue_write(sprintf("$repo/$relDir/palettes/%02d.pal", $_), $pal[$_]) for 0 .. $PALS_TOTAL - 1;
        push @gfxLines, "const u32 ${tilesVar}[] = INCGFX_U32(\"$relDir/tiles.png\", \".4bpp.lz\", \"-num_tiles $nTiles -Wnum_tiles\");", "",
                        "const u16 ${palVar}[][16] =", "{",
                        (map { sprintf("\tINCGFX_U16(\"%s/palettes/%02d.pal\", \".gbapal\"),", $relDir, $_) } 0 .. $PALS_TOTAL - 1),
                        "};", "";
    }
    queue_write("$repo/$relDir/metatiles.bin", pack('v*', @metatiles));
    queue_write("$repo/$relDir/metatile_attributes.bin", pack('v*', @attrs));
    $writtenTilesets{$oursym} = { dir => $relDir, nTiles => $nTiles, nMetatiles => scalar(@attrs), shared => $shareWith };
    push @metaLines, "const u16 ${metaVar}[] = INCBIN_U16(\"$relDir/metatiles.bin\");",
                     "const u16 ${attrVar}[] = INCBIN_U16(\"$relDir/metatile_attributes.bin\");", "";
    push @hdrLines, "const struct Tileset $oursym =", "{",
                    "    .isCompressed = TRUE,",
                    "    .isSecondary = " . ($t->{primary} ? 'FALSE' : 'TRUE') . ",",
                    "    .tiles = $tilesVar,",
                    "    .palettes = $palVar,",
                    "    .metatiles = $metaVar,",
                    "    .metatileAttributes = $attrVar,",
                    "    .callback = NULL, // tileset animations: PS-096",
                    "};", "";
}

for my $k (sort keys %behaviorUnknownRefs)
{
    err("$k: behaviour $behaviorUnknownRefs{$k} has no Emerald equivalent (behaviors.tsv)");
}
for my $n (sort keys %behaviorUnknownUnref)
{
    note("behaviour $n has no Emerald equivalent but only appears in metatiles no chosen map uses ($behaviorUnknownUnref{$n} metatiles, set to MB_NORMAL)");
}

my $genHeader = "// Generated by tools/kanto_import/kanto_import.pl ($CARDS) from pret/pokefirered data. Do not edit:\n// re-run the converter (tools/kanto_import/README.md).\n";
my $genBlockNote = "// Generated by tools/kanto_import/kanto_import.pl ($CARDS) from pret/pokefirered data: do not edit, re-run the converter.\n";

# layouts
my @layoutEntries;
for my $L (sort { $a->{id} cmp $b->{id} } values %layoutUsed)
{
    my $dirName = our_layout_dir($L->{name});
    my $cells = $L->{cells};
    my @body = @$cells[0 .. $L->{width} * $L->{height} - 1];
    my @border = @$cells[$L->{width} * $L->{height} .. $#$cells];
    queue_write("$repo/data/layouts/$dirName/map.bin", pack('v*', @body));
    queue_write("$repo/data/layouts/$dirName/border.bin", pack('v*', @border));
    $L->{ourId} = our_layout_id($L->{id});
    push @layoutEntries, join("\n",
        "{",
        "      \"id\": \"$L->{ourId}\",",
        "      \"name\": \"${dirName}_Layout\",",
        "      \"width\": $L->{width},",
        "      \"height\": $L->{height},",
        "      \"primary_tileset\": \"" . our_tileset_sym($L->{primary_tileset}, undef) . "\",",
        "      \"secondary_tileset\": \"" . our_tileset_sym($L->{secondary_tileset}, $L->{primary_tileset}) . "\",",
        "      \"border_filepath\": \"data/layouts/$dirName/border.bin\",",
        "      \"blockdata_filepath\": \"data/layouts/$dirName/map.bin\"",
        "    }");
}

# sections (FRLG order), heal locations, visited flags
my (@sectionIds, %sectionOf, %townSections, %healOf, @healEntries, %flagOf, %sectionMaps, %ourSecOfFrSec);
my %isTownType = (MAP_TYPE_TOWN => 1, MAP_TYPE_CITY => 1);
my %isOutdoorType = (MAP_TYPE_TOWN => 1, MAP_TYPE_CITY => 1, MAP_TYPE_ROUTE => 1, MAP_TYPE_OCEAN_ROUTE => 1);
for my $S (@{$frSections->{map_sections}})
{
    my @mapsHere = grep { $map{$_}{fr}{region_map_section} eq $S->{id} } @mapOrder;
    next unless @mapsHere;
    my $ours = our_section_id($S->{id});
    push @sectionIds, $ours;
    $sectionOf{$ours} = $S;
    $ourSecOfFrSec{$S->{id}} = $ours;
    $sectionMaps{$ours} = \@mapsHere;
    for my $m (@mapsHere)
    {
        if ($isTownType{$map{$m}{fr}{map_type}})
        {
            $townSections{$ours} = 1;
            if (my $h = $frHealByMap{$map{$m}{fr}{id}}) { $healOf{$ours} //= { map => $m, h => $h } }
        }
    }
}
for my $m (@mapOrder)
{
    err("map $m: section $map{$m}{fr}{region_map_section} is not in pokefirered's region_map_sections.json") unless $frSectionById{$map{$m}{fr}{region_map_section}};
}
# Map coordinates. Sections on pokefirered's cursor grid keep their x/y/width/height. The others
# (dungeons, indoor-only sections) carry x=0,y=0 in pokefirered: they get the cell of pokefirered's
# dungeon icon when there is one, else the cell of the first already placed map that warps into them
# (an outdoor map's warp position scaled to its section, an indoor map's own cell), so the cursor lands
# where the player is when the Fly map opens inside them. They are not placed on the cursor grid
# (the outdoor section stays under the cursor; Fly only needs the towns).
my %placed; # ours => [x, y, w, h]
for my $ours (@sectionIds)
{
    my $S = $sectionOf{$ours};
    if ($frOnMapLayer{$S->{id}} && defined $S->{x})
    {
        $placed{$ours} = [$S->{x}, $S->{y}, $S->{width} || 1, $S->{height} || 1];
        $S->{onGrid} = 1;
    }
    elsif (my $cell = $frDungeonCell{$S->{id}})
    {
        $placed{$ours} = [$cell->[0], $cell->[1], 1, 1];
        $S->{placedBy} = "pokefirered's dungeon icon";
    }
}
sub cell_of_map_at
{
    my ($m, $x, $y) = @_; # cursor cell of map $m at its tile ($x, $y)
    my $ours = $ourSecOfFrSec{$map{$m}{fr}{region_map_section}};
    my $p = $placed{$ours} or return;
    return [$p->[0], $p->[1]] unless $isOutdoorType{$map{$m}{fr}{map_type}};
    my $L = $frLayoutById{$map{$m}{fr}{layout}};
    my $cx = int($x * $p->[2] / $L->{width});  $cx = $p->[2] - 1 if $cx >= $p->[2];
    my $cy = int($y * $p->[3] / $L->{height}); $cy = $p->[3] - 1 if $cy >= $p->[3];
    return [$p->[0] + $cx, $p->[1] + $cy];
}
{
    my $progress = 1;
    while ($progress)
    {
        $progress = 0;
        for my $ours (@sectionIds)
        {
            next if $placed{$ours};
            my $S = $sectionOf{$ours};
            SEARCH: for my $m (@mapOrder)
            {
                next unless $placed{$ourSecOfFrSec{$map{$m}{fr}{region_map_section}}};
                for my $w (@{$map{$m}{fr}{warp_events} || []})
                {
                    my $destName = $frIdToName{$w->{dest_map}} or next;
                    next unless $map{$destName}{fr}{region_map_section} eq $S->{id};
                    my $cell = cell_of_map_at($m, $w->{x}, $w->{y}) or next;
                    $placed{$ours} = [$cell->[0], $cell->[1], 1, 1];
                    $S->{placedBy} = "the warp from $m";
                    $progress = 1;
                    last SEARCH;
                }
            }
        }
    }
}
for my $ours (@sectionIds)
{
    my $S = $sectionOf{$ours};
    unless ($placed{$ours})
    {
        $placed{$ours} = [$S->{x} // 0, $S->{y} // 0, $S->{width} || 1, $S->{height} || 1];
        note("section $ours is on neither of pokefirered's map layers and no placed map warps into it; it keeps pokefirered's coordinates ($placed{$ours}[0],$placed{$ours}[1])");
    }
    my $p = $placed{$ours};
    err("section $ours: cells ($p->[0]..@{[$p->[0] + $p->[2] - 1]},$p->[1]..@{[$p->[1] + $p->[3] - 1]}) do not fit the ${REGION_MAP_WIDTH}x$REGION_MAP_HEIGHT region map")
        if $p->[0] + $p->[2] > $REGION_MAP_WIDTH || $p->[1] + $p->[3] > $REGION_MAP_HEIGHT;
}
my $flagOffset = $FIRST_FLAG_OFFSET;
for my $sec (grep { $townSections{$_} } @sectionIds)
{
    $flagOf{$sec} = our_flag_id($sec);
    $flagOf{"$sec.offset"} = $flagOffset++;
}
my @sectionEntries;
for my $i (0 .. $#sectionIds)
{
    my $ours = $sectionIds[$i];
    my $S = $sectionOf{$ours};
    my $p = $placed{$ours};
    my $how = $S->{onGrid} ? "on the cursor grid" : "off the grid, placed by " . ($S->{placedBy} // "pokefirered's coordinates");
    my @lines = ("{", "      \"id\": \"$ours\",", "      \"comment\": \"$GENERATED_MARK: generated by tools/kanto_import from pokefirered $S->{id}; $how\",");
    push @lines, "      \"value\": \"$FIRST_REGION_MAPSEC\"," if $i == 0;
    push @lines, "      \"name\": \"$S->{name}\",";
    push @lines, "      \"x\": $p->[0],", "      \"y\": $p->[1],", "      \"width\": $p->[2],", "      \"height\": $p->[3]";
    push @lines, "    }";
    push @sectionEntries, join("\n", @lines);
}
# heal locations: every imported map that has one in pokefirered (towns' spots and the Route 4 / Route 10
# Pokémon Centers); Fly spots use the towns' ones
for my $h (@{$frHeal->{heal_locations}})
{
    my $m = $frIdToName{$h->{map}} or next;
    my $id = our_heal_id($h->{id});
    push @healEntries, join("\n", "{",
        "      \"id\": \"$id\",",
        "      \"map\": \"" . our_map_id($h->{map}) . "\",",
        "      \"x\": $h->{x},",
        "      \"y\": $h->{y}",
        "    }");
    for my $sec (grep { $healOf{$_} && $healOf{$_}{h} == $h } keys %healOf) { $healOf{$sec}{ourId} = $id }
}

# maps
my (@scriptIncludes, %warpCount);
$warpCount{$_} = scalar(@{$map{$_}{fr}{warp_events} || []}) for @mapOrder;
sub jbool { return $_[0] ? 'true' : 'false' }
sub jstr  { my $s = shift; $s =~ s/(["\\])/\\$1/g; return "\"$s\"" }
my %opposite = (up => 'down', down => 'up', left => 'right', right => 'left', dive => 'emerge', emerge => 'dive');
my %auditWarps = (checked => 0, inside => 0, redirected => 0, bad => 0);
my %auditConns = (checked => 0, kept => 0, dropped => 0, mismatch => 0, oneway => 0, standin => 0);
sub is_standin_map
{
    my $m = shift;
    my $fr = $map{$m}{fr};
    return $m =~ /_Connection$/ && !@{$fr->{warp_events} || []} && !@{$fr->{object_events} || []} && !@{$fr->{coord_events} || []} && !@{$fr->{bg_events} || []};
}
my (%warpRedirectTargets, %connDropTargets, %extrasUsed);
for my $m (@mapOrder)
{
    my $fr = $map{$m}{fr};
    my $name = our_map_name($m);
    my $id = our_map_id($fr->{id});
    my $sec = our_section_id($fr->{region_map_section});
    my $music = translate_music($fr->{music});
    my $weather = translate_const($fr->{weather}, $ourWeather, "map $m: weather");
    my $mapType = translate_const($fr->{map_type}, $ourMapTypes, "map $m: map type");
    my $scene = translate_const($fr->{battle_scene}, $ourMapTypes, "map $m: battle scene");
    note("map $m: floor_number $fr->{floor_number} dropped (Emerald headers have none)") if ($fr->{floor_number} // 0) != 0;
    for my $cat (qw(object_events coord_events bg_events))
    {
        my $n = scalar(@{$fr->{$cat} || []});
        note("map $m: $n $cat dropped (scripts, NPCs, signs and items are not imported)") if $n;
    }

    my @conns;
    for my $c (@{$fr->{connections} || []})
    {
        $auditConns{checked}++;
        if (my $destName = $frIdToName{$c->{map}})
        {
            push @conns, "    {\n      \"map\": \"" . our_map_id($c->{map}) . "\",\n      \"offset\": $c->{offset},\n      \"direction\": \"$c->{direction}\"\n    }";
            $auditConns{kept}++;
            # the other side: the same pair in the opposite direction with the negated offset
            my $opp = $opposite{$c->{direction}} // '';
            my ($back) = grep { $_->{direction} eq $opp && $_->{map} eq $fr->{id} } @{$map{$destName}{fr}{connections} || []};
            if (!$back)
            {
                $auditConns{oneway}++;
                note("map $m: connection $c->{direction} to $destName has no connection back (one-way, as in pokefirered)");
            }
            elsif ($back->{offset} != -$c->{offset})
            {
                # pokefirered's SaffronCity_Connection is a display-only stand-in for Saffron City seen
                # from its routes (no warps, no events, the routes' gates block the border): its own
                # offsets disagree with the routes' in pokefirered itself, which is kept as it is
                if (grep { is_standin_map($_) } ($m, $destName))
                {
                    $auditConns{standin}++;
                    note("map $m: connection $c->{direction} to $destName has offset $c->{offset} but the way back has $back->{offset}: kept, one of them is pokefirered's display-only stand-in map");
                }
                else
                {
                    $auditConns{mismatch}++;
                    err("map $m: connection $c->{direction} to $destName has offset $c->{offset} but the way back has $back->{offset} (expected " . (-$c->{offset}) . ")");
                }
            }
        }
        else
        {
            $auditConns{dropped}++;
            $connDropTargets{$c->{map}}++;
            note("map $m: connection $c->{direction} to $c->{map} dropped (not imported)");
        }
    }
    my @warps;
    my $wi = 0;
    for my $w (@{$fr->{warp_events} || []})
    {
        my ($destMap, $destId) = ($w->{dest_map}, $w->{dest_warp_id});
        $auditWarps{checked}++;
        if (my $destName = $frIdToName{$destMap})
        {
            $destMap = our_map_id($destMap);
            if ($destId !~ /^\d+$/ || $destId >= $warpCount{$destName})
            {
                $auditWarps{bad}++;
                err("map $m: warp $wi goes to $destName warp $destId but that map has only $warpCount{$destName} warps");
            }
            else { $auditWarps{inside}++ }
        }
        elsif ($destMap eq 'MAP_DYNAMIC' || $frAllMapIds{$destMap})
        {
            $auditWarps{redirected}++;
            $warpRedirectTargets{$destMap}++;
            note("map $m: warp $wi to $destMap redirected to itself (not imported)");
            $destMap = $id; $destId = "$wi";
        }
        else
        {
            $auditWarps{bad}++;
            err("map $m: warp $wi goes to $destMap, which is no pokefirered map");
        }
        push @warps, "    {\n      \"x\": $w->{x},\n      \"y\": $w->{y},\n      \"elevation\": $w->{elevation},\n      \"dest_map\": \"$destMap\",\n      \"dest_warp_id\": " . jstr($destId) . "\n    }";
        $wi++;
    }
    my $objects = "[]";
    if (my $text = $extras{$m}{'objects.json'})
    {
        my $parsed = eval { decode_json($text) };
        if (!$parsed || ref $parsed ne 'ARRAY') { err("extras: $m.objects.json is not a JSON array") }
        else
        {
            for my $o (@$parsed)
            {
                for my $k (qw(graphics_id x y elevation movement_type script flag)) { err("extras: $m.objects.json: an object event lacks \"$k\"") unless defined $o->{$k} }
                err("extras: $m.objects.json: script $o->{script} is not defined in $m.scripts.inc") if defined $o->{script} && $o->{script} ne '0x0' && ($extras{$m}{'scripts.inc'} // '') !~ /^\Q$o->{script}\E::/m;
            }
            $text =~ s/^\s+|\s+$//g;
            $objects = $text;
            $extrasUsed{$m}{objects} = scalar @$parsed;
        }
    }
    my $json = join("\n",
        "{",
        "  \"id\": \"$id\",",
        "  \"name\": \"$name\",",
        "  \"layout\": \"" . our_layout_id($fr->{layout}) . "\",",
        "  \"music\": \"$music\",",
        "  \"region_map_section\": \"$sec\",",
        "  \"requires_flash\": " . jbool($fr->{requires_flash}) . ",",
        "  \"weather\": \"$weather\",",
        "  \"map_type\": \"$mapType\",",
        "  \"allow_cycling\": " . jbool($fr->{allow_cycling}) . ",",
        "  \"allow_escaping\": " . jbool($fr->{allow_escaping}) . ",",
        "  \"allow_running\": " . jbool($fr->{allow_running}) . ",",
        "  \"show_map_name\": " . jbool($fr->{show_map_name}) . ",",
        "  \"battle_scene\": \"$scene\",",
        "  \"connections\": " . (@conns ? "[\n" . join(",\n", @conns) . "\n  ]" : "null") . ",",
        "  \"object_events\": $objects,",
        "  \"warp_events\": " . (@warps ? "[\n" . join(",\n", @warps) . "\n  ]" : "[]") . ",",
        "  \"coord_events\": [],",
        "  \"bg_events\": []",
        "}") . "\n";
    queue_write("$repo/data/maps/$name/map.json", $json);

    my $scripts = "@ Generated by tools/kanto_import/kanto_import.pl ($CARDS) from pokefirered data/maps/$m.\n"
                . "@ No NPCs, signs, items or story (epic PS-093): only what a walkable map needs.\n";
    if ($isTownType{$fr->{map_type}} && $flagOf{$sec})
    {
        $scripts .= "${name}_MapScripts::\n\tmap_script MAP_SCRIPT_ON_TRANSITION, ${name}_OnTransition\n\t.byte 0\n\n"
                  . "${name}_OnTransition:\n\tsetflag $flagOf{$sec}\n\tend\n";
    }
    else
    {
        $scripts .= "${name}_MapScripts::\n\t.byte 0\n";
    }
    if (my $extra = $extras{$m}{'scripts.inc'})
    {
        $scripts .= "\n@ Hand-written part from tools/kanto_import/extras/$m.scripts.inc (copied verbatim by the converter):\n" . $extra;
        $scripts .= "\n" unless $extra =~ /\n$/;
        $extrasUsed{$m}{scripts} = 1;
    }
    queue_write("$repo/data/maps/$name/scripts.inc", $scripts);
    push @scriptIncludes, "\t.include \"data/maps/$name/scripts.inc\"";
}
exit_if_errors('translating maps');

# ---------------------------------------------------------------------------------------------
# Repository files edited in place (text surgery keeps every other line byte for byte)
# ---------------------------------------------------------------------------------------------
# find the span of the array value of "key" starting at or after $from; returns (openIdx, closeIdx)
sub json_array_span
{
    my ($text, $key) = @_;
    my $k = index($text, "\"$key\":");
    die "no \"$key\" array in file\n" if $k < 0;
    my $open = index($text, '[', $k);
    my ($depth, $inStr, $i) = (0, 0, $open);
    for (; $i < length $text; $i++)
    {
        my $ch = substr($text, $i, 1);
        if ($inStr) { if ($ch eq '\\') { $i++ } elsif ($ch eq '"') { $inStr = 0 } next }
        if ($ch eq '"') { $inStr = 1 }
        elsif ($ch eq '[' || $ch eq '{') { $depth++ }
        elsif ($ch eq ']' || $ch eq '}') { $depth--; return ($open, $i) if $depth == 0 }
    }
    die "unbalanced \"$key\" array\n";
}
# split the inside of an array into its top-level elements (text, trimmed)
sub json_array_elements
{
    my $inner = shift;
    my @els;
    my ($depth, $inStr, $start) = (0, 0, undef);
    for (my $i = 0; $i < length $inner; $i++)
    {
        my $ch = substr($inner, $i, 1);
        if ($inStr) { if ($ch eq '\\') { $i++ } elsif ($ch eq '"') { $inStr = 0 } next }
        if ($ch =~ /\s/ && $depth == 0 && !defined $start) { next }
        if ($ch eq ',' && $depth == 0) { push @els, substr($inner, $start, $i - $start) if defined $start; $start = undef; next }
        $start //= $i;
        if ($ch eq '"') { $inStr = 1 }
        elsif ($ch eq '[' || $ch eq '{') { $depth++ }
        elsif ($ch eq ']' || $ch eq '}') { $depth-- }
    }
    push @els, substr($inner, $start) if defined $start;
    s/\s+$// for @els;
    return @els;
}
# replace the elements matching $dropRe with @new (object entries indented like the file's)
sub replace_array_entries
{
    my ($text, $key, $dropRe, $new, $indent) = @_;
    my ($open, $close) = json_array_span($text, $key);
    my @kept = grep { $_ !~ $dropRe } json_array_elements(substr($text, $open + 1, $close - $open - 1));
    my %keptIds = map { /"id": "(\w+)"/ ? ($1 => 1) : () } @kept;
    for my $e (@$new) { err("\"$key\": generated id $1 already exists in the repository file") if $e =~ /"id": "(\w+)"/ && $keptIds{$1} }
    my @all = (@kept, @$new);
    my $body = @all ? "\n" . join(",\n", map { "$indent$_" } @all) . "\n" . substr($indent, 2) : '';
    return substr($text, 0, $open) . '[' . $body . ']' . substr($text, $close + 1);
}

my %edits; # path => new content

# layouts.json
{
    my $path = "$repo/data/layouts/layouts.json";
    $edits{$path} = replace_array_entries(slurp($path), 'layouts', qr/"id": "LAYOUT_${UPREFIX}_/, \@layoutEntries, '    ');
}
# region_map_sections.json
{
    my $path = "$repo/src/data/region_map/region_map_sections.json";
    my $text = slurp($path);
    $edits{$path} = replace_array_entries($text, 'map_sections', qr/"comment": "\Q$GENERATED_MARK\E/, \@sectionEntries, '    ');
    my ($open, $close) = json_array_span($edits{$path}, 'map_sections');
    my @others = grep { /"value"/ && !/"comment": "\Q$GENERATED_MARK\E/ } json_array_elements(substr($edits{$path}, $open + 1, $close - $open - 1));
    note("region_map_sections.json: another section carries an explicit \"value\"; Kanto's first section claims $FIRST_REGION_MAPSEC, check the order") if @others;
}
# heal_locations.json
{
    my $path = "$repo/src/data/heal_locations.json";
    $edits{$path} = replace_array_entries(slurp($path), 'heal_locations', qr/"id": "HEAL_LOCATION_${UPREFIX}_/, \@healEntries, '    ');
}
# map_groups.json: group_order entries, the group arrays, nothing else
{
    my $path = "$repo/data/maps/map_groups.json";
    my $text = slurp($path);
    my @newGroups = map { our_group_name($_) } @groupOrder;
    $text = replace_array_entries($text, 'group_order', qr/^"gMapGroup_${PREFIX}/, [ map { "\"$_\"" } @newGroups ], '    ');
    $text =~ s/\n  "gMapGroup_${PREFIX}\w+": \[[^\]]*\],//g;
    my $block = '';
    for my $g (@groupOrder)
    {
        $block .= "\n  \"" . our_group_name($g) . "\": [\n" . join(",\n", map { "    \"" . our_map_name($_) . "\"" } @{$groupMaps{$g}}) . "\n  ],";
    }
    my $anchor = "\n  \"connections_include_order\"";
    die "map_groups.json: no connections_include_order key to insert the Kanto groups before\n" unless index($text, $anchor) >= 0;
    substr($text, index($text, $anchor), 0) = $block;
    $edits{$path} = $text;
    # group indices for mp_presence
    my ($open, $close) = json_array_span($text, 'group_order');
    my @order = map { s/"//g; $_ } json_array_elements(substr($text, $open + 1, $close - $open - 1));
    my (@sizes, @checks);
    for my $g (@groupOrder)
    {
        my $ours = our_group_name($g);
        my ($idx) = grep { $order[$_] eq $ours } 0 .. $#order;
        my $last = our_map_id($map{$groupMaps{$g}[-1]}{fr}{id});
        push @sizes, "    MP_GROUP_SIZE($last), /* $ours */ \\";
        push @checks, "    MP_CHECK_GROUP($idx, $last); \\";
    }
    $sizes[-1] =~ s/ \\$//; $checks[-1] =~ s/ \\$//;
    queue_write("$repo/src/mp/kanto_mp_groups.h", $genHeader
        . "// One MP_GROUP_SIZE entry and one MP_CHECK_GROUP per Kanto map group, in map_groups.json order\n"
        . "// (used by src/mp/mp_presence.c, see docs/regions.md step 10).\n\n"
        . "#define KANTO_MP_GROUP_SIZES \\\n" . join("\n", @sizes) . "\n\n"
        . "#define KANTO_MP_GROUP_CHECKS \\\n" . join("\n", @checks) . "\n");
}
# marked blocks: flags.h, event_scripts.s and the three Porymap-visible tileset files
sub replace_marked_block
{
    my ($path, $begin, $end, $body) = @_;
    my $text = slurp($path);
    my $b = index($text, $begin); my $e = index($text, $end);
    die "$path: markers '$begin' / '$end' not found (one-time hook, see tools/kanto_import/README.md)\n" if $b < 0 || $e < 0 || $e < $b;
    my $lineEnd = index($text, "\n", $b);
    $edits{$path} = substr($text, 0, $lineEnd + 1) . $body . substr($text, $e);
}
{
    my @lines;
    for my $sec (grep { $townSections{$_} } @sectionIds)
    {
        push @lines, sprintf("#define %-43s (REGION_FLAGS_START + 0x%X) // Fly spot / visited flag of %s", $flagOf{$sec}, $flagOf{"$sec.offset"}, $sectionOf{$sec}{name});
    }
    my $endValue = $flagOffset > $FIRST_FLAG_OFFSET ? $flagOf{(grep { $townSections{$_} } @sectionIds)[-1]} : sprintf("(REGION_FLAGS_START + 0x%X)", $FIRST_FLAG_OFFSET - 1);
    push @lines, sprintf("#define %-43s %s", "KANTO_FLAGS_END", $endValue);
    replace_marked_block("$repo/include/constants/flags.h", "// KANTO_IMPORT_BEGIN", "// KANTO_IMPORT_END", join("\n", @lines) . "\n");
    replace_marked_block("$repo/data/event_scripts.s", "@ KANTO_IMPORT_BEGIN", "@ KANTO_IMPORT_END", join("\n", @scriptIncludes) . "\n");
    replace_marked_block("$repo/src/data/tilesets/graphics.h", "// KANTO_IMPORT_BEGIN", "// KANTO_IMPORT_END", $genBlockNote . join("\n", @gfxLines) . "\n");
    replace_marked_block("$repo/src/data/tilesets/metatiles.h", "// KANTO_IMPORT_BEGIN", "// KANTO_IMPORT_END", $genBlockNote . join("\n", @metaLines) . "\n");
    replace_marked_block("$repo/src/data/tilesets/headers.h", "// KANTO_IMPORT_BEGIN", "// KANTO_IMPORT_END", $genBlockNote . join("\n", @hdrLines) . "\n");
}

# region map layout and Fly spots (the picture is kanto_region_map.pl's)
{
    my @grid = map { [ ('N') x $REGION_MAP_WIDTH ] } 1 .. $REGION_MAP_HEIGHT;
    for my $ours (@sectionIds)
    {
        my $S = $sectionOf{$ours};
        next unless $S->{onGrid};
        my $p = $placed{$ours};
        for my $dy (0 .. $p->[3] - 1)
        {
            for my $dx (0 .. $p->[2] - 1)
            {
                my ($x, $y) = ($p->[0] + $dx, $p->[1] + $dy);
                next if $x >= $REGION_MAP_WIDTH || $y >= $REGION_MAP_HEIGHT; # reported above
                err("region map cell ($x,$y) is claimed by both $grid[$y][$x] and $ours") if $grid[$y][$x] ne 'N';
                $grid[$y][$x] = $ours;
            }
        }
    }
    my @fly = map { "    { $_, $healOf{$_}{ourId}, $flagOf{$_} }," } grep { $healOf{$_} && $healOf{$_}{ourId} && $flagOf{$_} } @sectionIds;
    push @fly, "    { MAPSEC_NONE, HEAL_LOCATION_NONE, 0 }, // no Fly spot in the imported maps" unless @fly;
    my $h = $genHeader
        . "// Region map layout of REGION_KANTO (the map section under each cursor cell, coordinates from\n"
        . "// pokefirered's region_map_sections.json; the picture behind it is graphics/pokenav/region_map/kanto_map.*\n"
        . "// from tools/kanto_import/kanto_region_map.pl), its Fly spots, and its section id range.\n\n"
        . "#define MAPSEC_${UPREFIX}_FIRST $sectionIds[0]\n#define MAPSEC_${UPREFIX}_LAST  $sectionIds[-1]\n\n"
        . "#define N MAPSEC_NONE\nstatic const mapsec_t sKantoRegionMapLayout[REGION_MAP_HEIGHT][REGION_MAP_WIDTH] = {\n"
        . join("", map { "    {" . join(", ", @$_) . "},\n" } @grid)
        . "};\n#undef N\n\n"
        . "static const struct RegionFlySpot sKantoRegionFlySpots[] =\n{\n" . join("\n", @fly) . "\n};\n";
    queue_write("$repo/src/data/region_map/kanto_region.h", $h);
    note("graphics/pokenav/region_map/kanto_map.bin is missing: run tools/kanto_import/kanto_region_map.pl for the Kanto picture")
        unless -f "$repo/graphics/pokenav/region_map/kanto_map.bin";
}
exit_if_errors('generating region data');

# ---------------------------------------------------------------------------------------------
# Phase 2: write
# ---------------------------------------------------------------------------------------------
if ($dryRun)
{
    info("dry run: would write " . scalar(keys %pendingWrites) . " generated files and edit " . scalar(keys %edits) . " repository files");
}
else
{
    # the converter owns every Kanto* / kanto_* directory: remove them so a smaller map list leaves no stale output
    for my $d (glob("$repo/data/maps/$PREFIX*"), glob("$repo/data/layouts/$PREFIX*"), glob("$repo/data/tilesets/primary/" . lc($PREFIX) . "_*"), glob("$repo/data/tilesets/secondary/" . lc($PREFIX) . "_*"))
    {
        remove_tree($d) if -d $d;
    }
    # PS-094 wrote the tileset data to separate files; the marker blocks replaced them (PS-095)
    for my $old (qw(kanto_graphics.h kanto_metatiles.h kanto_headers.h)) { unlink "$repo/src/data/tilesets/$old" if -f "$repo/src/data/tilesets/$old" }
    flush_writes();
    for my $path (sort keys %edits)
    {
        open my $h, '>', $path or die "cannot write $path: $!\n";
        binmode $h; print $h $edits{$path}; close $h;
    }
    for my $hook (["$repo/src/region.c", 'kanto_region.h'], ["$repo/src/mp/mp_presence.c", 'KANTO_MP_GROUP_SIZES'])
    {
        note("$hook->[0] does not reference $hook->[1]: add the one-time hook (tools/kanto_import/README.md)") unless slurp($hook->[0]) =~ /\Q$hook->[1]\E/;
    }
}

# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------
my $nPrimary = scalar(grep { $_->[0] && !defined $_->[1] } @tilesetOutputs);
info(sprintf("kanto_import: %d maps in %d groups, %d layouts, %d tilesets (%d primary, %d sharing graphics), %d map sections (%d on the cursor grid), %d Fly spots, %d visited flags, %d heal locations",
     scalar(@mapOrder), scalar(@groupOrder), scalar(keys %layoutUsed), scalar(keys %writtenTilesets), $nPrimary, scalar(grep { $writtenTilesets{$_}{shared} } keys %writtenTilesets),
     scalar(@sectionIds), scalar(grep { $sectionOf{$_}{onGrid} } @sectionIds), scalar(grep { $healOf{$_} && $healOf{$_}{ourId} && $flagOf{$_} } @sectionIds), scalar(grep { $townSections{$_} } @sectionIds), scalar(@healEntries)));
for my $sym (sort keys %writtenTilesets) { my $w = $writtenTilesets{$sym}; info("  tileset $sym: $w->{dir} ($w->{nTiles} tiles, $w->{nMetatiles} metatiles" . ($w->{shared} ? ", graphics of " . our_tileset_sym($w->{shared}, undef) : "") . ")") }
info("  behaviours on the imported maps: " . join(", ", map { "$_" } sort keys %behaviorUsed));
for my $m (sort keys %extrasUsed) { info("  extras: $m: " . join(", ", ($extrasUsed{$m}{objects} ? "$extrasUsed{$m}{objects} object event(s)" : ()), ($extrasUsed{$m}{scripts} ? "scripts appended" : ()))) }
info("  note: $_") for @notes;
# the audits (every one of them fails the run above when something is wrong; this is the summary)
my $fmtTargets = sub { my $h = shift; return join(", ", map { "$_ x$h->{$_}" } sort keys %$h) };
info("audit: warps: $auditWarps{checked} checked, $auditWarps{inside} inside the import (target map and warp id exist), $auditWarps{redirected} redirected to their own map (targets outside the import: " . ($fmtTargets->(\%warpRedirectTargets) || 'none') . "), $auditWarps{bad} bad");
info("audit: connections: $auditConns{checked} checked, $auditConns{kept} kept with both sides agreeing on the offset, $auditConns{oneway} one-way (no connection back, as in pokefirered), $auditConns{standin} with pokefirered's own offset disagreement at its display-only stand-in map, $auditConns{dropped} dropped (targets outside the import: " . ($fmtTargets->(\%connDropTargets) || 'none') . "), $auditConns{mismatch} offset mismatches");
info("audit: layouts: " . scalar(keys %layoutUsed) . " layouts, $cellsChecked cells (borders included): every metatile id inside its primary or secondary tileset");
info("audit: music: " . scalar(@mapOrder) . " maps, " . scalar(keys %musicUsed) . " tracks, all known (" . join(", ", map { "$_ x$musicUsed{$_}" } sort keys %musicUsed) . ")");
info("audit: behaviours: " . scalar(keys %behaviorUsed) . " distinct on the imported maps, every one translated (" . scalar(grep { exists $behaviorTable->{$_} } keys %behaviorUsed) . " through behaviors.tsv, the rest by name)");
exit 0;

sub exit_if_errors
{
    my $stage = shift;
    return unless @errors;
    print STDERR "kanto_import: $_\n" for @errors;
    if ($allowUnknown && $stage !~ /map list|loading tilesets/)
    {
        print STDERR "kanto_import: " . scalar(@errors) . " problem(s) while $stage, continuing because of --allow-unknown\n";
        @errors = ();
        return;
    }
    print STDERR "kanto_import: " . scalar(@errors) . " problem(s) while $stage, nothing written\n";
    exit 1;
}
