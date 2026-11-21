#!/usr/bin/perl
# tools/kanto_import/measure.pl -- PS-094 measurements behind the Kanto tileset strategy: FRLG tileset usage
# versus Emerald limits, layouts with primary metatiles >= 512, metatile behaviours used, map.json fields.
#   perl tools/kanto_import/measure.pl <pokefirered checkout> <repository root>
# Read-only; the pokefirered files are data.
use strict; use warnings; use JSON::PP; use File::Basename;
my ($FR, $OURS) = @ARGV;
sub slurp { my $f = shift; open my $h, '<', $f or die "open $f: $!"; binmode $h; local $/; my $d = <$h>; close $h; $d }
sub rjson { decode_json(slurp(shift)) }

# ---- behaviours: FRLG names (defines), ours (enum) ----
my (%frName, %ourId);
for (split /\n/, slurp("$FR/include/constants/metatile_behaviors.h")) {
    $frName{hex $2} = $1 if /^#define (MB_\w+) (0x[0-9A-Fa-f]+)/;
}
{
    my $i = 0;
    for (split /\n/, slurp("$OURS/include/constants/metatile_behaviors.h")) {
        if (/^\s+(MB_\w+),/) { $ourId{$1} = $i++; }
    }
}

# ---- tileset header names -> dir ----
my %tsDir; # gTileset_X -> {dir, primary}
for my $kind (qw(primary secondary)) {
    for my $d (glob("$FR/data/tilesets/$kind/*")) {
        my $b = basename($d);
        my $camel = join "", map { ucfirst } split /_/, $b; $camel = "SSAnne" if $b eq "ss_anne";
        $tsDir{"gTileset_$camel"} = { dir => $d, primary => $kind eq 'primary', base => $b };
    }
}
my $hdr = slurp("$FR/src/data/tilesets/headers.h");
my %known = map { $_ => 1 } ($hdr =~ /const struct Tileset (gTileset_\w+)/g);
for my $k (sort keys %known) { warn "no dir guessed for $k\n" unless $tsDir{$k}; }

sub loadTileset {
    my $name = shift; my $t = $tsDir{$name} or die "unknown tileset $name";
    return $t if $t->{loaded};
    my $mt = slurp("$t->{dir}/metatiles.bin");
    my $at = slurp("$t->{dir}/metatile_attributes.bin");
    $t->{nMeta} = length($mt) / 16;
    $t->{tiles} = [ unpack("v*", $mt) ];
    $t->{attrs} = [ unpack("V*", $at) ];
    $t->{loaded} = 1;
    if (-f "$t->{dir}/tiles.png") { my $png = slurp("$t->{dir}/tiles.png"); my ($w, $h) = unpack("N N", substr($png, 16, 8)); $t->{nTiles} = ($w / 8) * ($h / 8); } else { $t->{nTiles} = -1; }
    my @p = glob("$t->{dir}/palettes/*.pal"); $t->{nPals} = scalar @p;
    return $t;
}

# ---- per-primary tileset analysis ----
print "=== FRLG primary tilesets ===\n";
for my $name (sort grep { $tsDir{$_}{primary} } keys %tsDir) {
    my $t = loadTileset($name);
    my (%tilesLow, %tilesHigh, %palsLow, %palsHigh, %palsAll);
    my $maxTile = 0; my $lowUsesHighTile = 0; my %hiTilesByLow;
    for my $m (0 .. $t->{nMeta} - 1) {
        for my $k (0 .. 7) {
            my $e = $t->{tiles}[$m * 8 + $k]; my $tile = $e & 0x3ff; my $pal = ($e >> 12) & 0xf;
            $maxTile = $tile if $tile > $maxTile;
            $palsAll{$pal}++;
            if ($m < 512) { $tilesLow{$tile}++; $palsLow{$pal}++; if ($tile >= 512) { $lowUsesHighTile++; $hiTilesByLow{$tile}++ } }
            else { $tilesHigh{$tile}++; $palsHigh{$pal}++; }
        }
    }
    my $nonBlankHigh = 0;
    for my $m (512 .. $t->{nMeta} - 1) { my $blank = 1; for my $k (0..7) { $blank = 0 if ($t->{tiles}[$m*8+$k] & 0x3ff) != 0 } $nonBlankHigh++ unless $blank; }
    my %distinctTiles = (%tilesLow, %tilesHigh);
    my @hi = sort { $a <=> $b } grep { $_ >= 512 } keys %distinctTiles;
    printf "%s: tiles.png=%d tiles, metatiles=%d (non-blank in 512..639: %d), palettes=%d files\n", $name, $t->{nTiles}, $t->{nMeta}, $nonBlankHigh, $t->{nPals};
    printf "  distinct tile ids referenced=%d, of which >=512: %d (max tile id %d)\n", scalar(keys %distinctTiles), scalar(@hi), $maxTile;
    printf "  metatiles <512 referencing tiles >=512: %d tile refs (%d distinct tiles)\n", $lowUsesHighTile, scalar(keys %hiTilesByLow);
    printf "  palettes referenced by metatiles <512: %s ; by 512..639: %s\n", join(',', sort { $a <=> $b } keys %palsLow), join(',', sort { $a <=> $b } keys %palsHigh);
    printf "  palette 6 (FRLG primary slot 7, Emerald's first secondary slot) refs: %d\n", $palsAll{6} // 0;
    my $attrHigh = 0; for my $m (512 .. $t->{nMeta}-1) { $attrHigh++ if $t->{attrs}[$m] }
    printf "  metatiles 512..639 with non-zero attributes: %d\n", $attrHigh;
    printf "  tile ids >=512 referenced: %s\n", join(',', @hi);
}

# ---- secondary tilesets ----
print "\n=== FRLG secondary tilesets (tile id range / palettes referenced / metatiles / png tiles / pal files) ===\n";
for my $name (sort grep { !$tsDir{$_}{primary} } keys %tsDir) {
    my $t = loadTileset($name);
    my ($minT, $maxT, %pals) = (9999, 0);
    for my $i (0 .. $t->{nMeta}*8 - 1) { my $e = $t->{tiles}[$i]; my $tile = $e & 0x3ff; next if $tile == 0; $minT = $tile if $tile < $minT; $maxT = $tile if $tile > $maxT; $pals{($e>>12)&0xf}++; }
    printf "%-28s tiles %4d..%4d  pals %-16s  metatiles %3d  png tiles %d  palfiles %d\n", $name, $minT, $maxT, join(',', sort { $a<=>$b } keys %pals), $t->{nMeta}, $t->{nTiles}, $t->{nPals};
}

# ---- maps ----
my $layouts = rjson("$FR/data/layouts/layouts.json");
my $groups = rjson("$FR/data/maps/map_groups.json");
my (%mapLayout, %mapSec, %mapInfo, %allKeys, %evKeys, %evTypes, %layoutMaps);
for my $g (@{$groups->{group_order}}) {
    for my $m (@{$groups->{$g}}) {
        my $j = rjson("$FR/data/maps/$m/map.json");
        $mapLayout{$m} = $j->{layout}; $mapSec{$m} = $j->{region_map_section}; $mapInfo{$m} = $j;
        push @{$layoutMaps{$j->{layout}}}, $m;
        $allKeys{$_}++ for keys %$j;
        for my $cat (qw(object_events warp_events coord_events bg_events)) {
            for my $e (@{$j->{$cat} || []}) { my $ty = $e->{type} // 'warp'; $evTypes{"$cat/$ty"}++; $evKeys{"$cat/$ty"}{$_}++ for keys %$e; }
        }
        for my $c (@{$j->{connections} || []}) { $evKeys{"connections"}{$_}++ for keys %$c; }
    }
}
my @slice = qw(PalletTown PalletTown_PlayersHouse_1F PalletTown_PlayersHouse_2F PalletTown_ProfessorOaksLab PalletTown_RivalsHouse Route1 ViridianCity ViridianCity_Gym ViridianCity_House ViridianCity_Mart ViridianCity_PokemonCenter_1F ViridianCity_PokemonCenter_2F ViridianCity_School);
my %sliceLayouts = map { $mapLayout{$_} => 1 } @slice;
# Sevii: map groups whose name mentions Island, plus Sevii dungeons by section name
my %seviiSec;
for my $m (keys %mapSec) {
    my $s = $mapSec{$m} // '';
    $seviiSec{$m} = 1 if $s =~ /ISLAND|ISLE|KINDLE|TREASURE|TANOBY|NAVEL|BIRTH|BERRY_FOREST|ALTERING|CAPE_BRINK|BOND_BRIDGE|WATER_PATH|RUIN_VALLEY|TRAINER_TOWER|CANYON|SEVAULT|OUTCAST|GREEN_PATH|LABYRINTH|RESORT|MEMORIAL|ICEFALL|ROCKET_WAREHOUSE|PATTERN_BUSH|DOTTED_HOLE|LOST_CAVE|MT_EMBER|EMBER_SPA|HARBOR|VIAPOIS|MONEAN|LIPTOO|WEEPTH|DILFORD|SCUFIB|RIXY/;
}
my %kantoLayout; for my $m (keys %mapLayout) { $kantoLayout{$mapLayout{$m}} = 1 unless $seviiSec{$m}; }

print "\n=== layouts referencing primary metatile ids >= 512 (cells incl. border) ===\n";
my ($nLayouts, $nOver) = (0, 0); my (%overIdsByPrim, %overLayoutsByPrim);
my (%behAll, %behSlice, %behKanto, %layerAll, %terrainAll, %encAll, %behLayouts, %behTerrain);
for my $L (@{$layouts->{layouts}}) {
    $nLayouts++;
    next unless $tsDir{$L->{primary_tileset} // ""} && $tsDir{$L->{secondary_tileset} // ""};
    my $p = loadTileset($L->{primary_tileset}); my $s = loadTileset($L->{secondary_tileset});
    my $bin = slurp("$FR/$L->{blockdata_filepath}"); my $bord = slurp("$FR/$L->{border_filepath}");
    my @cells = (unpack("v*", $bin), unpack("v*", $bord));
    my %over;
    for my $c (@cells) {
        my $id = $c & 0x3ff;
        if ($id >= 512 && $id < 640) { $over{$id}++ }
        my $attr = $id < 640 ? $p->{attrs}[$id] : $s->{attrs}[$id - 640];
        if (!defined $attr) { warn "$L->{id}: metatile $id out of range for $L->{secondary_tileset} (n=$s->{nMeta})\n"; next; }
        my $beh = $attr & 0x1ff; my $terrain = ($attr >> 9) & 0x1f; my $enc = ($attr >> 24) & 7; my $layer = ($attr >> 29) & 3;
        $behAll{$beh}++; $behLayouts{$beh}{$L->{id}}++; $layerAll{$layer}++; $terrainAll{$terrain}++; $encAll{$enc}++; $behTerrain{$beh}{"t$terrain/e$enc"}++;
        $behSlice{$beh}++ if $sliceLayouts{$L->{id}};
        $behKanto{$beh}++ if $kantoLayout{$L->{id}};
        my $other = $attr & ~(0x1ff | 0x3e00 | 0x07000000 | 0x60000000);
        warn sprintf("%s: metatile %d has bits outside behavior/terrain/encounter/layer: 0x%08x\n", $L->{id}, $id, $attr) if $other;
    }
    if (%over) {
        $nOver++;
        $overLayoutsByPrim{$L->{primary_tileset}}{$L->{id}} = 1;
        $overIdsByPrim{$L->{primary_tileset}}{$_} += $over{$_} for keys %over;
        my $n = 0; $n += $_ for values %over;
        printf "  %-45s %-18s %4d cells, ids: %s%s\n", $L->{id}, $L->{primary_tileset}, $n, join(',', sort { $a<=>$b } keys %over), $sliceLayouts{$L->{id}} ? '  [SLICE]' : '';
    }
}
printf "layouts total=%d, with >=512 primary refs=%d\n", $nLayouts, $nOver;
for my $pr (sort keys %overIdsByPrim) {
    printf "  %s: %d layouts, %d distinct overflow metatile ids: %s\n", $pr, scalar(keys %{$overLayoutsByPrim{$pr}}), scalar(keys %{$overIdsByPrim{$pr}}), join(',', sort { $a<=>$b } keys %{$overIdsByPrim{$pr}});
}

print "\n=== metatile behaviours used (all FRLG layouts incl. Sevii / Kanto only / slice) ===\n";
printf "%-36s %5s %8s %8s %8s  %s\n", 'FRLG name', 'id', 'all', 'kanto', 'slice', 'Emerald';
for my $b (sort { $a <=> $b } keys %behAll) {
    my $n = $frName{$b} // sprintf("MB_UNKNOWN_%02X", $b);
    my $em = exists $ourId{$n} ? sprintf("same name (0x%02X)", $ourId{$n}) : 'NOT IN EMERALD';
    printf "%-36s 0x%03X %8d %8d %8d  %s   [%s]\n", $n, $b, $behAll{$b}, $behKanto{$b} // 0, $behSlice{$b} // 0, $em, join(' ', map { "$_=$behTerrain{$b}{$_}" } sort keys %{$behTerrain{$b}});
}
print "\nlayer types used: ", join(', ', map { "$_=$layerAll{$_}" } sort keys %layerAll), "\n";
print "terrain types used: ", join(', ', map { "$_=$terrainAll{$_}" } sort keys %terrainAll), "\n";
print "encounter types used: ", join(', ', map { "$_=$encAll{$_}" } sort keys %encAll), "\n";

print "\n=== map.json top-level keys (all FRLG maps) ===\n", join(', ', sort keys %allKeys), "\n";
print "=== event fields ===\n";
for my $k (sort keys %evKeys) { print "$k: ", join(', ', sort keys %{$evKeys{$k}}), "\n"; }
print "event type counts: ", join(', ', map { "$_=$evTypes{$_}" } sort keys %evTypes), "\n";
my $kantoMaps = grep { !$seviiSec{$_} } keys %mapLayout;
printf "maps=%d (kanto-ish %d, sevii-ish %d) layouts=%d (kanto %d)\n", scalar(keys %mapLayout), $kantoMaps, scalar(keys %seviiSec), $nLayouts, scalar(keys %kantoLayout);
print "\n=== slice layouts ===\n";
for my $m (@slice) { my $L = (grep { $_->{id} eq $mapLayout{$m} } @{$layouts->{layouts}})[0]; printf "%-32s %-36s %3dx%-3d %s + %s\n", $m, $L->{id}, $L->{width}, $L->{height}, $L->{primary_tileset}, $L->{secondary_tileset}; }
