#!/usr/bin/env perl
# PS-097: Kanto wild encounters from pret/pokefirered's src/data/wild_encounters.json.
#
# Reads pokefirered's wild_encounters.json, keeps the FireRed tables (base_label *_FireRed; the
# LeafGreen ones are dropped) of the maps listed in --maps (the PS-095 import list; Sevii, Navel
# Rock, Birth Island and every other map outside the import are dropped), renames the maps to our
# MAP_KANTO_* ids and writes the entries into src/data/wild_encounters.json, in the single
# gWildMonHeaders group (the engine's GetCurrentMapWildMonHeaderId walks only that array), after
# Hoenn's entries. The Hoenn text is left byte for byte; a Kanto entry is one whose "map" starts
# with MAP_KANTO_, so a re-run replaces the previous Kanto entries (idempotent). Every map, species,
# level range, table size and encounter rate is checked against this repository before anything is
# written; a miss prints and exits 1 without writing.
#
# Perl core modules only (JSON::PP). Nothing from the pokefirered checkout is executed; its files are
# read as data. See README.md in this directory.
use strict;
use warnings;
use File::Basename qw(basename dirname);
use File::Spec;
use Getopt::Long;
use JSON::PP;

my $scriptDir = dirname(File::Spec->rel2abs($0));
my ($frlg, $repo, $mapsFile, $dryRun, $quiet);
$repo = File::Spec->rel2abs(File::Spec->catdir($scriptDir, '..', '..'));
$mapsFile = File::Spec->catfile($scriptDir, 'maps_ps095.txt');
GetOptions('frlg=s' => \$frlg, 'repo=s' => \$repo, 'maps=s' => \$mapsFile, 'dry-run' => \$dryRun, 'quiet' => \$quiet)
    or die "usage: $0 --frlg <pokefirered dir> [--repo <dir>] [--maps <file>] [--dry-run] [--quiet]\n";
die "--frlg <pokefirered checkout> is required\n" unless defined $frlg && -d $frlg;
die "$frlg has no src/data/wild_encounters.json\n" unless -f "$frlg/src/data/wild_encounters.json";

my (@errors, @notes);
sub err  { push @errors, shift }
sub note { push @notes, shift }
sub info { print @_, "\n" unless $quiet }
sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; return $d }
sub rjson { return JSON::PP->new->decode(slurp(shift)) }
sub camel { my $s = shift; return join '', map { ucfirst lc } split /_/, $s }

my $GROUP = 'gWildMonHeaders';
my $OUR_PREFIX = 'MAP_KANTO_';
my $LABEL_PREFIX = 'gKanto';
my @TYPES = qw(land_mons water_mons rock_smash_mons fishing_mons); # the order of our json entries

# ---------------------------------------------------------------------------------------------
# Our data: wild_encounters.json, species, maps
# ---------------------------------------------------------------------------------------------
my $ourPath = "$repo/src/data/wild_encounters.json";
my $ourText = slurp($ourPath);
my $ours = JSON::PP->new->decode($ourText);
my @ourGroups = grep { $_->{label} eq $GROUP } @{$ours->{wild_encounter_groups} || []};
die "$ourPath: expected exactly one $GROUP group with for_maps (the engine reads that array for the maps; the Battle Pyramid / Pike groups are not per map)\n"
    unless @ourGroups == 1 && $ourGroups[0]{for_maps};
my $ourGroup = $ourGroups[0];
my %ourField = map { $_->{type} => $_ } @{$ourGroup->{fields}};
my %slots = map { $_ => scalar @{$ourField{$_}{encounter_rates}} } keys %ourField;

my %species;
for (split /\n/, slurp("$repo/include/constants/species.h")) { $species{$1} = 1 if /^#define (SPECIES_\w+)/ }
die "no SPECIES_* defines found in include/constants/species.h\n" unless keys %species > 300;

my @mapNames;
for (split /\n/, slurp($mapsFile)) { s/#.*//; s/^\s+|\s+$//g; push @mapNames, $_ if length }
die "no maps listed in $mapsFile\n" unless @mapNames;
my %chosen = map { $_ => 1 } @mapNames;

# ---------------------------------------------------------------------------------------------
# pokefirered data
# ---------------------------------------------------------------------------------------------
my $fr = rjson("$frlg/src/data/wild_encounters.json");
my ($frGroup) = grep { $_->{label} eq $GROUP } @{$fr->{wild_encounter_groups}};
die "pokefirered's wild_encounters.json has no $GROUP group\n" unless $frGroup;
note("pokefirered's wild_encounters.json has " . scalar(@{$fr->{wild_encounter_groups}}) . " groups; only $GROUP is imported")
    if @{$fr->{wild_encounter_groups}} > 1;

# the slot tables (encounter_rates per slot, the rod groups) are compiled from OUR json; FRLG's must be the same
{
    my %frField = map { $_->{type} => $_ } @{$frGroup->{fields}};
    my $enc = JSON::PP->new->canonical;
    for my $type (sort keys %ourField)
    {
        if (!$frField{$type}) { err("pokefirered has no \"$type\" field definition") }
        elsif ($enc->encode($frField{$type}) ne $enc->encode($ourField{$type})) { err("field \"$type\": pokefirered's encounter_rates / rod groups differ from ours") }
    }
    for my $type (sort keys %frField) { err("pokefirered defines field \"$type\", which our json does not") unless $ourField{$type} }
}

# FRLG map id -> map name (data/maps/<name>)
my $frGroups = rjson("$frlg/data/maps/map_groups.json");
my (%frMapName, %frMap);
for my $g (@{$frGroups->{group_order}})
{
    for my $m (@{$frGroups->{$g}}) { my $j = rjson("$frlg/data/maps/$m/map.json"); $frMapName{$j->{id}} = $m; $frMap{$m} = $j }
}

# ---------------------------------------------------------------------------------------------
# Select and translate
# ---------------------------------------------------------------------------------------------
my %ourLabels = map { $_->{base_label} => 1 } grep { $_->{map} !~ /^\Q$OUR_PREFIX\E/ } @{$ourGroup->{encounters}};
my (%drop, %kept, @entries, %usedLabel, %speciesSeen, %tableCount);
my %mapEntry; # FRLG map name => the selected FRLG entry (for the tile audit)
for my $e (@{$frGroup->{encounters}})
{
    my ($map, $label) = ($e->{map}, $e->{base_label});
    if ($label =~ /_LeafGreen$/) { $drop{'LeafGreen table'}++; next }
    my $name = $frMapName{$map};
    unless (defined $name) { err("$label: $map is no pokefirered map"); next }
    unless ($chosen{$name}) { $drop{$name =~ /Island|MtEmber|Sevault|Tanoby|Altering|TrainerTower|NavelRock|BirthIsland/ ? 'Sevii / event island map, not imported' : 'map not in the import list'}++; next }
    my $ourMapPath = "$repo/data/maps/Kanto$name/map.json";
    unless (-f $ourMapPath) { err("$label: $name is in the map list but data/maps/Kanto$name does not exist (run kanto_import.pl first)"); next }
    my $ourMapId = rjson($ourMapPath)->{id};
    unless (defined $ourMapId && $ourMapId =~ /^\Q$OUR_PREFIX\E\w+$/) { err("$label: data/maps/Kanto$name/map.json has no $OUR_PREFIX* id"); next }
    if ($mapEntry{$name}) { err("$label: $map has two FireRed tables"); next }

    my $ourLabel = $label =~ /^s(\w+?)(?:_FireRed)?$/ ? "$LABEL_PREFIX$1" : $LABEL_PREFIX . camel($ourMapId =~ s/^\Q$OUR_PREFIX\E//r);
    err("$label: generated label $ourLabel already exists in our json") if $ourLabels{$ourLabel};
    err("$label: generated label $ourLabel is used twice") if $usedLabel{$ourLabel}++;

    my $new = { map => $ourMapId, base_label => $ourLabel };
    my $nTables = 0;
    for my $key (sort keys %$e)
    {
        next if $key eq 'map' || $key eq 'base_label';
        unless ($slots{$key}) { err("$label: unknown table \"$key\""); next }
        my $t = $e->{$key};
        my $rate = $t->{encounter_rate};
        err("$label/$key: encounter_rate " . ($rate // 'missing') . " is not 0..255") unless defined $rate && $rate =~ /^\d+$/ && $rate <= 255;
        my @mons = @{$t->{mons} || []};
        err("$label/$key: " . scalar(@mons) . " slots, the engine expects $slots{$key}") unless @mons == $slots{$key};
        my @newMons;
        for my $m (@mons)
        {
            my ($lo, $hi, $sp) = ($m->{min_level}, $m->{max_level}, $m->{species});
            err("$label/$key: level range " . ($lo // '?') . "..." . ($hi // '?') . " is not sane (1..100, min <= max)")
                unless defined $lo && defined $hi && $lo =~ /^\d+$/ && $hi =~ /^\d+$/ && $lo >= 1 && $hi <= 100 && $lo <= $hi;
            if (!defined $sp || !$species{$sp}) { err("$label/$key: species " . ($sp // 'missing') . " does not exist in include/constants/species.h") }
            else { $speciesSeen{$sp}++ }
            push @newMons, { min_level => $lo, max_level => $hi, species => $sp };
        }
        $new->{$key} = { encounter_rate => $rate, mons => \@newMons };
        $tableCount{$key}++;
        $nTables++;
    }
    err("$label: no tables at all") unless $nTables;
    push @entries, $new;
    $mapEntry{$name} = $new;
    $kept{$name} = 1;
}

# ---------------------------------------------------------------------------------------------
# Audit: which cells of the imported layouts are encounter tiles for OUR engine. pokefirered
# triggers encounters from a per-metatile attribute (ENCOUNTER_TYPE land / water) that Emerald's
# attribute format does not have; kanto_import.pl drops it and Emerald decides from the metatile
# behaviour (sTileBitAttributes in src/metatile_behavior.c: TILE_FLAG_HAS_ENCOUNTERS, surfable or
# not). So a FRLG land cell gives encounters here only if its behaviour translates to one of
# Emerald's encounter behaviours (tall grass, cave, indoor encounter, ...). Reported, not fatal.
# ---------------------------------------------------------------------------------------------
{
    my $frFieldmap = slurp("$frlg/include/fieldmap.h");
    my ($PRIMARY) = $frFieldmap =~ /^#define NUM_METATILES_IN_PRIMARY (\d+)/m;
    die "cannot read NUM_METATILES_IN_PRIMARY from pokefirered's include/fieldmap.h\n" unless $PRIMARY;
    my ($encMask, $encShift) = (0x07000000, 24); # METATILE_ATTRIBUTE_ENCOUNTER_TYPE, pokefirered src/fieldmap.c
    my ($behMask) = (0x000001FF);
    my ($ENC_LAND, $ENC_WATER) = (1, 2);           # TILE_ENCOUNTER_LAND / _WATER, pokefirered include/global.fieldmap.h

    my %frBehName; # id => MB_X (pokefirered)
    for (split /\n/, slurp("$frlg/include/constants/metatile_behaviors.h")) { $frBehName{hex $2} = $1 if /^#define (MB_\w+) (0x[0-9A-Fa-f]+)/ }
    my %ourBeh;    # MB_X => 1 (ours)
    for (split /\n/, slurp("$repo/include/constants/metatile_behaviors.h")) { $ourBeh{$1} = 1 if /^\s+(MB_\w+),/ }
    my %tsv;       # FRLG MB_X => our MB_Y (behaviors.tsv)
    for (split /\n/, slurp("$scriptDir/behaviors.tsv")) { next if /^#/ || !/\S/; my ($f, $o) = split /\t/; $tsv{$f} = $o if defined $o }
    my (%hasEnc, %surfable); # our MB_X => flag
    for (split /\n/, slurp("$repo/src/metatile_behavior.c"))
    {
        my ($beh, $flags) = /^\s+\[(MB_\w+)\]\s*=\s*(.*?),/ or next;
        $hasEnc{$beh} = 1 if $flags =~ /TILE_FLAG_HAS_ENCOUNTERS/;
        $surfable{$beh} = 1 if $flags =~ /TILE_FLAG_SURFABLE/;
    }
    die "no TILE_FLAG_HAS_ENCOUNTERS behaviours found in src/metatile_behavior.c\n" unless keys %hasEnc;

    my %tsDir; # gTileset_X => dir
    for my $kind (qw(primary secondary))
    {
        for my $d (glob("$frlg/data/tilesets/$kind/*")) { next unless -d $d; my $b = basename($d); $tsDir{'gTileset_' . ($b eq 'ss_anne' ? 'SSAnne' : camel($b))} = $d }
    }
    my %attrs; # gTileset_X => [u32 attributes]
    my $attrsOf = sub { my $sym = shift; $attrs{$sym} //= [ unpack('V*', slurp("$tsDir{$sym}/metatile_attributes.bin")) ]; return $attrs{$sym} };
    my $frLayouts = rjson("$frlg/data/layouts/layouts.json");
    my %frLayoutById = map { $_->{id} => $_ } grep { defined $_->{id} } @{$frLayouts->{layouts}};

    # a cell counts only if it is passable (collision bits 10-11 clear): pokefirered sets the encounter
    # type on walls, ledges and warps too, where nobody can stand
    my ($tLand, $tLandOk, $tWater, $tWaterOk, $tExtra, $tBlocked) = (0, 0, 0, 0, 0, 0);
    my (%keptPairs, %lostPairs, %extraPairs, @lostLand, @lostWater, @noLand, @noWater, %noFrLand);
    for my $name (sort keys %mapEntry)
    {
        my $e = $mapEntry{$name};
        my $lay = $frLayoutById{$frMap{$name}{layout}} or do { note("audit: $name: layout $frMap{$name}{layout} not in pokefirered's layouts.json"); next };
        my ($pri, $sec) = ($lay->{primary_tileset}, $lay->{secondary_tileset});
        unless ($tsDir{$pri} && $tsDir{$sec}) { note("audit: $name: tileset directory for $pri / $sec not found"); next }
        my ($pa, $sa) = ($attrsOf->($pri), $attrsOf->($sec));
        my @cells = unpack('v*', slurp("$frlg/$lay->{blockdata_filepath}"));
        my ($land, $landOk, $water, $waterOk, $extra) = (0, 0, 0, 0, 0);
        for my $c (@cells)
        {
            my $id = $c & 0x3FF;
            my $a = $id < $PRIMARY ? $pa->[$id] : $sa->[$id - $PRIMARY];
            next unless defined $a;
            my $frEnc = ($a & $encMask) >> $encShift;
            next unless $frEnc || $hasEnc{$tsv{$frBehName{$a & $behMask} // ''} // ''} || $hasEnc{$frBehName{$a & $behMask} // ''};
            if (($c >> 10) & 3) { $tBlocked++; next }
            my $frB = $frBehName{$a & $behMask} // sprintf('MB_0x%02X', $a & $behMask);
            my $ourB = $tsv{$frB} // ($ourBeh{$frB} ? $frB : '-');
            my $ourLand = $hasEnc{$ourB} && !$surfable{$ourB};
            my $ourWater = $hasEnc{$ourB} && $surfable{$ourB};
            if ($frEnc == $ENC_LAND) { $land++; if ($ourLand) { $landOk++; $keptPairs{"$frB -> $ourB"}++ } else { $lostPairs{"$frB -> $ourB"}++ } }
            elsif ($frEnc == $ENC_WATER) { $water++; if ($ourWater) { $waterOk++; $keptPairs{"$frB -> $ourB"}++ } else { $lostPairs{"$frB -> $ourB"}++ } }
            elsif ($ourLand || $ourWater) { $extra++; $extraPairs{"$frB -> $ourB"}++ }
        }
        $tLand += $land; $tLandOk += $landOk; $tWater += $water; $tWaterOk += $waterOk; $tExtra += $extra;
        push @lostLand, "$name " . ($land - $landOk) . "/$land" if $landOk < $land;
        push @lostWater, "$name " . ($water - $waterOk) . "/$water" if $waterOk < $water;
        push @noLand, $name if $e->{land_mons} && !$landOk && $land;
        $noFrLand{$name} = 1 if $e->{land_mons} && !$land;
        push @noWater, $name if $e->{water_mons} && !$waterOk && $water;
    }
    my $fmt = sub { my $h = shift; return join(', ', map { "$_ x$h->{$_}" } sort keys %$h) || 'none' };
    info("audit: encounter tiles on the " . scalar(keys %mapEntry) . " maps with tables (passable cells only; $tBlocked blocked cells carry an encounter type in pokefirered and are ignored): "
        . "$tLand pokefirered land cells, $tLandOk of them encounter tiles here; $tWater water cells, $tWaterOk here; $tExtra cells that are encounter tiles here but not in pokefirered");
    info("audit: kept (pokefirered behaviour -> ours): " . $fmt->(\%keptPairs));
    info("audit: lost (pokefirered behaviour -> ours): " . $fmt->(\%lostPairs));
    info("audit: gained (pokefirered behaviour -> ours): " . $fmt->(\%extraPairs));
    info("audit: maps losing land cells (lost/pokefirered): " . (join(', ', @lostLand) || 'none'));
    info("audit: maps losing water cells (lost/pokefirered): " . (join(', ', @lostWater) || 'none'));
    info("audit: maps whose land table pokefirered itself never reaches (no passable land cell): " . (join(', ', sort keys %noFrLand) || 'none'));
    note("audit: maps with a land table but no land encounter tile here: @noLand") if @noLand;
    note("audit: maps with a water table but no water encounter tile here: @noWater") if @noWater;
}

# ---------------------------------------------------------------------------------------------
# Report, then write
# ---------------------------------------------------------------------------------------------
info("note: $_") for @notes;
if (@errors)
{
    print STDERR "error: $_\n" for @errors;
    print STDERR scalar(@errors) . " error(s), nothing written\n";
    exit 1;
}
info("kanto_encounters: " . scalar(@entries) . " maps with FireRed tables kept (" . join(', ', map { "$_ $tableCount{$_}" } grep { $tableCount{$_} } @TYPES)
    . "), " . scalar(keys %speciesSeen) . " species; dropped: " . join(', ', map { "$_ x$drop{$_}" } sort keys %drop));

# JSON text in the file's own style (2-space steps, one key per line), entries indented like the Hoenn ones
sub fmt_mon   { my ($m, $ind) = @_; return "$ind\{\n$ind  \"min_level\": $m->{min_level},\n$ind  \"max_level\": $m->{max_level},\n$ind  \"species\": \"$m->{species}\"\n$ind}" }
sub fmt_table { my ($t, $ind) = @_; return "{\n$ind  \"encounter_rate\": $t->{encounter_rate},\n$ind  \"mons\": [\n" . join(",\n", map { fmt_mon($_, "$ind    ") } @{$t->{mons}}) . "\n$ind  ]\n$ind}" }
sub fmt_entry
{
    my ($e, $ind) = @_;
    my @lines = ("$ind  \"map\": \"$e->{map}\"", "$ind  \"base_label\": \"$e->{base_label}\"");
    push @lines, "$ind  \"$_\": " . fmt_table($e->{$_}, "$ind  ") for grep { $e->{$_} } @TYPES;
    return "{\n" . join(",\n", @lines) . "\n$ind}";
}
# the span of the "encounters" array of our group: (openIdx, closeIdx) in $ourText
sub encounters_span
{
    my $text = shift;
    my $g = index($text, "\"label\": \"$GROUP\"");
    die "no \"label\": \"$GROUP\" in $ourPath\n" if $g < 0;
    my $k = index($text, '"encounters":', $g);
    die "no \"encounters\" array after the $GROUP label\n" if $k < 0;
    my $open = index($text, '[', $k);
    my ($depth, $inStr) = (0, 0);
    for (my $i = $open; $i < length $text; $i++)
    {
        my $ch = substr($text, $i, 1);
        if ($inStr) { if ($ch eq '\\') { $i++ } elsif ($ch eq '"') { $inStr = 0 } next }
        if ($ch eq '"') { $inStr = 1 }
        elsif ($ch eq '[' || $ch eq '{') { $depth++ }
        elsif ($ch eq ']' || $ch eq '}') { $depth--; return ($open, $i) if $depth == 0 }
    }
    die "unbalanced \"encounters\" array\n";
}
# the top-level elements of an array's inside (text, trimmed)
sub array_elements
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

my ($open, $close) = encounters_span($ourText);
my $indent = ' ' x 8;
my @kept = grep { !/"map": "\Q$OUR_PREFIX\E/ } array_elements(substr($ourText, $open + 1, $close - $open - 1));
my @all = (@kept, map { fmt_entry($_, $indent) } @entries);
my $newText = substr($ourText, 0, $open) . "[\n" . join(",\n", map { "$indent$_" } @all) . "\n" . (' ' x 6) . ']' . substr($ourText, $close + 1);

# verify: valid JSON, Hoenn unchanged, Kanto as generated
{
    my $check = JSON::PP->new->decode($newText);
    my $enc = JSON::PP->new->canonical;
    my ($checkGroup) = grep { $_->{label} eq $GROUP } @{$check->{wild_encounter_groups}};
    my @hoennBefore = grep { $_->{map} !~ /^\Q$OUR_PREFIX\E/ } @{$ourGroup->{encounters}};
    my @hoennAfter = grep { $_->{map} !~ /^\Q$OUR_PREFIX\E/ } @{$checkGroup->{encounters}};
    my @kantoAfter = grep { $_->{map} =~ /^\Q$OUR_PREFIX\E/ } @{$checkGroup->{encounters}};
    die "verify: the non-Kanto entries changed\n" unless $enc->encode(\@hoennBefore) eq $enc->encode(\@hoennAfter);
    die "verify: the Kanto entries written differ from the generated ones\n" unless $enc->encode(\@kantoAfter) eq $enc->encode(\@entries);
    # everything else in the file (the other groups, our group's field definitions) must be the same
    my $rest = sub { my $j = shift; my @g = map { $_->{label} eq $GROUP ? { %$_, encounters => undef } : $_ } @{$j->{wild_encounter_groups}}; return $enc->encode({ %$j, wild_encounter_groups => \@g }) };
    die "verify: something outside the $GROUP encounters changed\n" unless $rest->($ours) eq $rest->($check);
    info("verify: " . scalar(@hoennAfter) . " non-Kanto entries unchanged, " . scalar(@kantoAfter) . " Kanto entries, " . scalar(@{$checkGroup->{encounters}}) . " in $GROUP; the other "
        . (scalar(@{$check->{wild_encounter_groups}}) - 1) . " group(s) unchanged");
}

if ($dryRun) { info("dry run: $ourPath not written" . ($newText eq $ourText ? ' (it is already up to date)' : '')) }
elsif ($newText eq $ourText) { info("$ourPath is already up to date") }
else
{
    open my $h, '>', $ourPath or die "cannot write $ourPath: $!\n";
    binmode $h;
    print $h $newText;
    close $h;
    info("wrote $ourPath");
}
exit 0;
