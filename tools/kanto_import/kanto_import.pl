#!/usr/bin/perl
# tools/kanto_import/kanto_import.pl -- PS-093/PS-094: import Kanto map data from a pret pokefirered
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
# secondary paired with it, so no metatile or tile id changes).
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
my $REGION_MAP_TILEMAP_SIDE = 64;   # graphics/pokenav/region_map/*.bin: 64x64 bytes
my $REGION_MAP_TOWN_TILE = 0x0D;    # Hoenn picture's inland town marker (docs/regions.md)
my $REGION_MAP_ROUTE_TILE = 0x4D;   # the template picture's route segment
my $REGION_MAP_CURSOR_X_MIN = 1;    # src/region_map.c MAPCURSOR_X_MIN / Y_MIN
my $REGION_MAP_CURSOR_Y_MIN = 2;
my $FIRST_REGION_MAPSEC = '0x100';  # MAPSEC_REGIONS_START: Kanto is the first region after Hoenn
my $FIRST_FLAG_OFFSET = 0x10;       # REGION_FLAGS_START + 0x00..0x0F are FLAG_BADGE09..24

my $PREFIX = 'Kanto';               # map / layout / tileset name prefix
my $UPREFIX = 'KANTO';              # constant prefix

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

my (@errors, @notes);
sub err  { push @errors, shift }
sub note { push @notes, shift }
sub info { print @_, "\n" unless $quiet }

# ---------------------------------------------------------------------------------------------
# File helpers
# ---------------------------------------------------------------------------------------------
sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; return $d }
sub rjson { return decode_json(slurp(shift)) }
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

sub translate_music
{
    my $mus = shift;
    (my $rg = $mus) =~ s/^MUS_/MUS_RG_/;
    return $rg if our_has_define($ourSongs, $rg);
    return $mus if our_has_define($ourSongs, $mus);
    return $constTable->{$mus}{to} if exists $constTable->{$mus} && our_has_define($ourSongs, $constTable->{$mus}{to});
    err("music $mus has no Emerald track (add a row to constants.tsv)");
    return 'MUS_NONE';
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
my %frTileset; # gTileset_X => { dir, base, primary }
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
    my $hdr = slurp("$frlg/src/data/tilesets/headers.h");
    for my $sym ($hdr =~ /const struct Tileset (gTileset_\w+)/g)
    {
        note("pokefirered tileset $sym has no data/tilesets directory with that name (fine unless a chosen map uses it)") unless $frTileset{$sym};
    }
}
sub fr_tileset
{
    my $sym = shift;
    my $t = $frTileset{$sym} or die "unknown pokefirered tileset $sym\n";
    return $t if $t->{loaded};
    $t->{loaded} = 1;
    for my $f (qw(metatiles.bin metatile_attributes.bin tiles.png))
    {
        unless (-f "$t->{dir}/$f")
        {
            err("tileset $sym: $t->{dir}/$f is missing (pokefirered shares another tileset's graphics for it; not supported, copy them by hand)");
            $t->{broken} = 1;
        }
    }
    unless (-d "$t->{dir}/palettes") { err("tileset $sym: no palettes directory"); $t->{broken} = 1 }
    return $t if $t->{broken};
    $t->{metatiles} = [ unpack('v*', slurp("$t->{dir}/metatiles.bin")) ];
    $t->{attrs} = [ unpack('V*', slurp("$t->{dir}/metatile_attributes.bin")) ];
    $t->{nMetatiles} = scalar(@{$t->{attrs}});
    die "tileset $sym: metatiles.bin and metatile_attributes.bin disagree\n" unless scalar(@{$t->{metatiles}}) == 8 * $t->{nMetatiles};
    $t->{png} = png_read("$t->{dir}/tiles.png");
    for my $i (0 .. $PALS_TOTAL - 1)
    {
        my $p = sprintf("%s/palettes/%02d.pal", $t->{dir}, $i);
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
my %frHealByMap; # MAP_X => entry
for my $h (@{$frHeal->{heal_locations}}) { $frHealByMap{$h->{map}} //= $h }

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
sub our_tileset_sym{ my $sym = shift; $sym =~ s/^gTileset_/gTileset_$PREFIX/; return $sym }
sub our_tileset_var{ my ($sym, $what) = @_; (my $s = $sym) =~ s/^gTileset_//; return "$what\_$PREFIX$s" } # gTilesetTiles_KantoGeneral
sub our_tileset_dir{ my $t = shift; return ($t->{primary} ? 'primary' : 'secondary') . '/' . lc($PREFIX) . "_$t->{base}" }
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
my (%layoutUsed, %tilesetPairs, %tilesetUsed);
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
    err("secondary tileset $s is used with several primaries (@p): the prefix re-pack needs one copy per primary, not supported yet") if @p > 1;
}
exit_if_errors('reading the map list');

# load tilesets
fr_tileset($_) for sort keys %tilesetUsed;
exit_if_errors('loading tilesets');

# metatile ids referenced by the chosen layouts, per tileset symbol and (FRLG) metatile id
my %referenced; # sym => { frMetatileId => 1 }
for my $L (sort { $a->{id} cmp $b->{id} } values %layoutUsed)
{
    my @cells = (unpack('v*', slurp("$frlg/$L->{blockdata_filepath}")), unpack('v*', slurp("$frlg/$L->{border_filepath}")));
    my $nCells = $L->{width} * $L->{height};
    err("layout $L->{id}: map.bin has " . (scalar(@cells) - 4) . " cells, expected $nCells") unless scalar(@cells) == $nCells + 4;
    my $s = fr_tileset($L->{secondary_tileset});
    my $secondaryCount = $s->{nMetatiles} > $MAX_SECONDARY_METATILES ? $MAX_SECONDARY_METATILES : $s->{nMetatiles};
    for my $c (@cells)
    {
        my $id = $c & 0x3FF;
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

# tilesets -> files
my (@gfxLines, @metaLines, @hdrLines);
my %writtenTilesets;
for my $sym (sort { ($frTileset{$b}{primary} <=> $frTileset{$a}{primary}) || ($a cmp $b) } keys %tilesetUsed)
{
    my $t = fr_tileset($sym);
    my $oursym = our_tileset_sym($sym);
    my $dir = our_tileset_dir($t);
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
        my ($psym) = keys %{$tilesetPairs{$sym}};
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
    queue_write("$repo/$relDir/tiles.png", png_build($PNG_WIDTH, $t->{png}{plte}, \@rows));
    queue_write("$repo/$relDir/metatiles.bin", pack('v*', @metatiles));
    queue_write("$repo/$relDir/metatile_attributes.bin", pack('v*', @attrs));
    queue_write(sprintf("$repo/$relDir/palettes/%02d.pal", $_), $pal[$_]) for 0 .. $PALS_TOTAL - 1;
    $writtenTilesets{$sym} = { oursym => $oursym, dir => $relDir, nTiles => $nTiles, nMetatiles => scalar(@attrs) };

    my $tilesVar = our_tileset_var($sym, 'gTilesetTiles');
    my $palVar = our_tileset_var($sym, 'gTilesetPalettes');
    my $metaVar = our_tileset_var($sym, 'gMetatiles');
    my $attrVar = our_tileset_var($sym, 'gMetatileAttributes');
    push @gfxLines, "const u32 ${tilesVar}[] = INCGFX_U32(\"$relDir/tiles.png\", \".4bpp.lz\", \"-num_tiles $nTiles -Wnum_tiles\");", "",
                    "const u16 ${palVar}[][16] =", "{",
                    (map { sprintf("\tINCGFX_U16(\"%s/palettes/%02d.pal\", \".gbapal\"),", $relDir, $_) } 0 .. $PALS_TOTAL - 1),
                    "};", "";
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

my $genHeader = "// Generated by tools/kanto_import/kanto_import.pl (PS-094) from pret/pokefirered data. Do not edit:\n// re-run the converter (tools/kanto_import/README.md).\n";
queue_write("$repo/src/data/tilesets/kanto_graphics.h", $genHeader . "\n" . join("\n", @gfxLines));
queue_write("$repo/src/data/tilesets/kanto_metatiles.h", $genHeader . "\n" . join("\n", @metaLines));
queue_write("$repo/src/data/tilesets/kanto_headers.h", $genHeader . "\n" . join("\n", @hdrLines));

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
        "      \"primary_tileset\": \"" . our_tileset_sym($L->{primary_tileset}) . "\",",
        "      \"secondary_tileset\": \"" . our_tileset_sym($L->{secondary_tileset}) . "\",",
        "      \"border_filepath\": \"data/layouts/$dirName/border.bin\",",
        "      \"blockdata_filepath\": \"data/layouts/$dirName/map.bin\"",
        "    }");
}

# sections (FRLG order), heal locations, visited flags
my (@sectionIds, %sectionOf, %townSections, %healOf, @healEntries, %flagOf);
my %isTownType = (MAP_TYPE_TOWN => 1, MAP_TYPE_CITY => 1);
for my $S (@{$frSections->{map_sections}})
{
    my @mapsHere = grep { $map{$_}{fr}{region_map_section} eq $S->{id} } @mapOrder;
    next unless @mapsHere;
    my $ours = our_section_id($S->{id});
    push @sectionIds, $ours;
    $sectionOf{$ours} = $S;
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
    my @lines = ("{", "      \"id\": \"$ours\",", "      \"comment\": \"$GENERATED_MARK: generated by tools/kanto_import from pokefirered $S->{id}\",");
    push @lines, "      \"value\": \"$FIRST_REGION_MAPSEC\"," if $i == 0;
    push @lines, "      \"name\": \"$S->{name}\"" . (defined $S->{x} ? "," : "");
    if (defined $S->{x})
    {
        push @lines, "      \"x\": $S->{x},", "      \"y\": $S->{y},", "      \"width\": $S->{width},", "      \"height\": $S->{height}";
    }
    push @lines, "    }";
    push @sectionEntries, join("\n", @lines);
}
for my $sec (grep { $healOf{$_} } @sectionIds)
{
    my $h = $healOf{$sec}{h};
    my $id = our_heal_id($h->{id});
    $healOf{$sec}{ourId} = $id;
    push @healEntries, join("\n", "{",
        "      \"id\": \"$id\",",
        "      \"map\": \"" . our_map_id($h->{map}) . "\",",
        "      \"x\": $h->{x},",
        "      \"y\": $h->{y}",
        "    }");
}

# maps
my (@scriptIncludes, %warpCount);
$warpCount{$_} = scalar(@{$map{$_}{fr}{warp_events} || []}) for @mapOrder;
sub jbool { return $_[0] ? 'true' : 'false' }
sub jstr  { my $s = shift; $s =~ s/(["\\])/\\$1/g; return "\"$s\"" }
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
        if (my $destName = $frIdToName{$c->{map}})
        {
            push @conns, "    {\n      \"map\": \"" . our_map_id($c->{map}) . "\",\n      \"offset\": $c->{offset},\n      \"direction\": \"$c->{direction}\"\n    }";
        }
        else
        {
            note("map $m: connection $c->{direction} to $c->{map} dropped (not imported)");
        }
    }
    my @warps;
    my $wi = 0;
    for my $w (@{$fr->{warp_events} || []})
    {
        my ($destMap, $destId) = ($w->{dest_map}, $w->{dest_warp_id});
        if (my $destName = $frIdToName{$destMap})
        {
            $destMap = our_map_id($destMap);
            err("map $m: warp $wi goes to $destName warp $destId but that map has only $warpCount{$destName} warps") if $destId =~ /^\d+$/ && $destId >= $warpCount{$destName};
        }
        else
        {
            note("map $m: warp $wi to $destMap redirected to itself (not imported)");
            $destMap = $id; $destId = "$wi";
        }
        push @warps, "    {\n      \"x\": $w->{x},\n      \"y\": $w->{y},\n      \"elevation\": $w->{elevation},\n      \"dest_map\": \"$destMap\",\n      \"dest_warp_id\": " . jstr($destId) . "\n    }";
        $wi++;
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
        "  \"object_events\": [],",
        "  \"warp_events\": " . (@warps ? "[\n" . join(",\n", @warps) . "\n  ]" : "[]") . ",",
        "  \"coord_events\": [],",
        "  \"bg_events\": []",
        "}") . "\n";
    queue_write("$repo/data/maps/$name/map.json", $json);

    my $scripts = "@ Generated by tools/kanto_import/kanto_import.pl (PS-094) from pokefirered data/maps/$m.\n"
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
# flags.h and event_scripts.s: marked blocks
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
}

# region map layout, Fly spots, placeholder picture
{
    my @grid = map { [ ('N') x $REGION_MAP_WIDTH ] } 1 .. $REGION_MAP_HEIGHT;
    my $tilemap = "\0" x ($REGION_MAP_TILEMAP_SIDE * $REGION_MAP_TILEMAP_SIDE);
    my %outdoor = (MAP_TYPE_TOWN => 1, MAP_TYPE_CITY => 1, MAP_TYPE_ROUTE => 1, MAP_TYPE_OCEAN_ROUTE => 1);
    for my $ours (@sectionIds)
    {
        my $S = $sectionOf{$ours};
        next unless defined $S->{x};
        next unless grep { $outdoor{$map{$_}{fr}{map_type}} && our_section_id($map{$_}{fr}{region_map_section}) eq $ours } @mapOrder;
        for my $dy (0 .. $S->{height} - 1)
        {
            for my $dx (0 .. $S->{width} - 1)
            {
                my ($x, $y) = ($S->{x} + $dx, $S->{y} + $dy);
                if ($x >= $REGION_MAP_WIDTH || $y >= $REGION_MAP_HEIGHT) { err("section $ours cell ($x,$y) is outside the ${REGION_MAP_WIDTH}x$REGION_MAP_HEIGHT region map"); next }
                $grid[$y][$x] = $ours;
                my $tile = $townSections{$ours} ? $REGION_MAP_TOWN_TILE : $REGION_MAP_ROUTE_TILE;
                substr($tilemap, ($y + $REGION_MAP_CURSOR_Y_MIN) * $REGION_MAP_TILEMAP_SIDE + $x + $REGION_MAP_CURSOR_X_MIN, 1) = chr($tile);
            }
        }
    }
    my @fly = map { "    { $_, $healOf{$_}{ourId}, $flagOf{$_} }," } grep { $healOf{$_} && $flagOf{$_} } @sectionIds;
    push @fly, "    { MAPSEC_NONE, HEAL_LOCATION_NONE, 0 }, // no Fly spot in the imported maps" unless @fly;
    my $h = $genHeader
        . "// Region map layout of REGION_KANTO (the map section under each cursor cell, coordinates from\n"
        . "// pokefirered's region_map_sections.json), its Fly spots, and its section id range.\n\n"
        . "#define MAPSEC_${UPREFIX}_FIRST $sectionIds[0]\n#define MAPSEC_${UPREFIX}_LAST  $sectionIds[-1]\n\n"
        . "#define N MAPSEC_NONE\nstatic const mapsec_t sKantoRegionMapLayout[REGION_MAP_HEIGHT][REGION_MAP_WIDTH] = {\n"
        . join("", map { "    {" . join(", ", @$_) . "},\n" } @grid)
        . "};\n#undef N\n\n"
        . "static const struct RegionFlySpot sKantoRegionFlySpots[] =\n{\n" . join("\n", @fly) . "\n};\n";
    queue_write("$repo/src/data/region_map/kanto_region.h", $h);
    queue_write("$repo/graphics/pokenav/region_map/kanto_map.bin", $tilemap);
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
    flush_writes();
    for my $path (sort keys %edits)
    {
        open my $h, '>', $path or die "cannot write $path: $!\n";
        binmode $h; print $h $edits{$path}; close $h;
    }
    for my $hook (["$repo/src/tilesets.c", 'kanto_headers.h'], ["$repo/src/region.c", 'kanto_region.h'], ["$repo/src/mp/mp_presence.c", 'KANTO_MP_GROUP_SIZES'])
    {
        note("$hook->[0] does not reference $hook->[1]: add the one-time hook (tools/kanto_import/README.md)") unless slurp($hook->[0]) =~ /\Q$hook->[1]\E/;
    }
}

# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------
info(sprintf("kanto_import: %d maps, %d layouts, %d tilesets (%d primary), %d map sections, %d Fly spots, %d visited flags, %d heal locations",
     scalar(@mapOrder), scalar(keys %layoutUsed), scalar(keys %writtenTilesets), scalar(grep { $frTileset{$_}{primary} } keys %writtenTilesets),
     scalar(@sectionIds), scalar(grep { $healOf{$_} && $flagOf{$_} } @sectionIds), scalar(grep { $townSections{$_} } @sectionIds), scalar(@healEntries)));
for my $sym (sort keys %writtenTilesets) { my $w = $writtenTilesets{$sym}; info("  tileset $w->{oursym}: $w->{dir} ($w->{nTiles} tiles, $w->{nMetatiles} metatiles)") }
info("  behaviours on the imported maps: " . join(", ", map { "$_" } sort keys %behaviorUsed));
info("  note: $_") for @notes;
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
