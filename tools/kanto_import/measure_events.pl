#!/usr/bin/perl
# tools/kanto_import/measure_events.pl -- PS-100: the numbers behind the Kanto story import (epic PS-099).
#
#   perl tools/kanto_import/measure_events.pl --frlg <pokefirered> --maps <list> [--repo <root>] [--verbose]
#
# Reads pokefirered's map.json events, map scripts and text, the shared scripts they pull in
# (data/scripts, data/text, data/event_scripts.s) and compares everything they reference with this
# repository: object events by kind and sprite, bg / coord events, movement types, flags, vars,
# specials, macros, trainers (classes, pics, music), items, songs, text placeholders. Nothing is
# written; the pokefirered files are data. Perl core modules only.
use strict;
use warnings;
use JSON::PP;
use File::Basename qw(dirname basename);
use File::Spec;
use Getopt::Long;

my ($frlg, $mapsFile, $repo, $verbose);
GetOptions('frlg=s' => \$frlg, 'maps=s' => \$mapsFile, 'repo=s' => \$repo, 'verbose' => \$verbose)
    or die "usage: $0 --frlg <pokefirered> --maps <list> [--repo <root>] [--verbose]\n";
die "--frlg <pokefirered checkout> is required\n" unless defined $frlg && -d "$frlg/data/maps";
die "--maps <list file> is required\n" unless defined $mapsFile && -f $mapsFile;
$repo //= File::Spec->rel2abs(File::Spec->catdir(dirname(__FILE__), '..', '..'));

sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; $d =~ s/\r\n/\n/g; return $d }
sub rjson { return JSON::PP->new->decode(slurp(shift)) }
sub defines { my ($file, $prefix) = @_; my %d; for (split /\n/, slurp($file)) { $d{$1} = 1 if /^#define\s+($prefix\w*)/ } return \%d }
sub uniq { my %s; return grep { !$s{$_}++ } @_ }
sub section { print "\n", "=" x 100, "\n$_[0]\n", "=" x 100, "\n" }
sub listing { my ($title, @items) = @_; print "  $title (", scalar(@items), "): ", (@items ? join(" ", @items) : "-"), "\n" }

# ---------------------------------------------------------------------------------------------
# Definitions on both sides
# ---------------------------------------------------------------------------------------------
my %fr = (
    flags    => defines("$frlg/include/constants/flags.h", 'FLAG_'),
    vars     => defines("$frlg/include/constants/vars.h", 'VAR_'),
    items    => defines("$frlg/include/constants/items.h", 'ITEM_'),
    gfx      => defines("$frlg/include/constants/event_objects.h", 'OBJ_EVENT_GFX_'),
    movement => defines("$frlg/include/constants/event_object_movement.h", 'MOVEMENT_TYPE_'),
    species  => defines("$frlg/include/constants/species.h", 'SPECIES_'),
    trainers => defines("$frlg/include/constants/opponents.h", 'TRAINER_'),
);
my %our = (
    flags    => defines("$repo/include/constants/flags.h", 'FLAG_'),
    vars     => defines("$repo/include/constants/vars.h", 'VAR_'),
    items    => defines("$repo/include/constants/items.h", 'ITEM_'),
    gfx      => defines("$repo/include/constants/event_objects.h", 'OBJ_EVENT_GFX_'),
    movement => defines("$repo/include/constants/event_object_movement.h", 'MOVEMENT_TYPE_'),
    species  => defines("$repo/include/constants/species.h", 'SPECIES_'),
    trainers => defines("$repo/include/constants/opponents.h", 'TRAINER_'),
    songs    => defines("$repo/include/constants/songs.h", '(?:MUS|SE)_'),
    classes  => defines("$repo/include/constants/trainers.h", 'TRAINER_CLASS_'),
    pics     => defines("$repo/include/constants/trainers.h", 'TRAINER_PIC_'),
    music    => defines("$repo/include/constants/trainers.h", 'TRAINER_ENCOUNTER_MUSIC_'),
);
my %frClasses = %{defines("$frlg/include/constants/trainers.h", 'TRAINER_CLASS_')};
sub specials { my %s; for (split /\n/, slurp(shift)) { $s{$1} = 1 if /def_special\s+(\w+)/ } return \%s }
my $frSpecials = specials("$frlg/data/specials.inc");
my $ourSpecials = specials("$repo/data/specials.inc");
sub macros { my %m; for (split /\n/, slurp(shift)) { $m{$1} = 1 if /^\s*\.macro\s+(\w+)/ } return \%m }
my $frMacros = macros("$frlg/asm/macros/event.inc");
my $ourMacros = macros("$repo/asm/macros/event.inc");
my %ourMovementMacros = (%{macros("$repo/asm/macros/movement.inc")}, %{macros("$repo/asm/macros/map.inc")}, %{macros("$repo/asm/macros.inc")});
# every CAPS identifier this repository defines anywhere in include/constants (for the "unknown constant" check)
my %ourConstants;
for my $f (glob("$repo/include/constants/*.h"), glob("$repo/include/constants/*/*.h"), "$repo/asm/macros/event.inc", "$repo/asm/macros/movement.inc")
{
    for (split /\n/, slurp($f)) { $ourConstants{$1} = 1 if /^\s*#define\s+([A-Z][A-Z0-9_]*)/ || /^\s*([A-Z][A-Z0-9_]*)\s*=/ || /^\s*\.set\s+([A-Z][A-Z0-9_]*)/ || /^\s+([A-Z][A-Z0-9_]+),?\s*(\/\/.*)?$/ }
}
my %ourCharmap;
for (split /\n/, slurp("$repo/charmap.txt")) { $ourCharmap{$1} = 1 if /^([A-Z][A-Z0-9_]*)\s*=/ }
my %frCharmap;
for (split /\n/, slurp("$frlg/charmap.txt")) { $frCharmap{$1} = $2 if /^([A-Z][A-Z0-9_]*)\s*=\s*(.*?)\s*$/ }

# this repository's script labels (to find name collisions with pokefirered's shared scripts)
my %ourLabels;
for my $f ("$repo/data/event_scripts.s", glob("$repo/data/scripts/*.inc"), glob("$repo/data/text/*.inc"), glob("$repo/data/maps/*/scripts.inc"))
{
    for (split /\n/, slurp($f)) { $ourLabels{$1} = 1 if /^(\w+)::?\s*(\/\*.*|@.*)?$/ }
}

# pokefirered trainers: id -> { class, music, pic, name, party }
my %frTrainer;
{
    my $t = slurp("$frlg/src/data/trainers.h");
    while ($t =~ /\[(TRAINER_\w+)\]\s*=\s*\{(.*?)\n\s*\},/sg)
    {
        my ($id, $body) = ($1, $2);
        my %e = (id => $id);
        $e{class} = $1 if $body =~ /\.trainerClass\s*=\s*(\w+)/;
        $e{music} = $1 if $body =~ /\.encounterMusic_gender\s*=\s*([^,]+),/;
        $e{pic}   = $1 if $body =~ /\.trainerPic\s*=\s*(\w+)/;
        $e{name}  = $1 if $body =~ /\.trainerName\s*=\s*_\("([^"]*)"\)/;
        $e{party} = $1 if $body =~ /\.party\s*=\s*\w+\((\w+)\)/;
        $e{double} = ($body =~ /\.doubleBattle\s*=\s*TRUE/) ? 1 : 0;
        $frTrainer{$id} = \%e;
    }
}

# ---------------------------------------------------------------------------------------------
# pokefirered script sources: label -> { file, lines }, file -> equs
# ---------------------------------------------------------------------------------------------
my (%label, %fileOfLabel, %equ);
sub index_script_file
{
    my ($file, $scope) = @_;
    return unless -f $file;
    my $cur;
    for my $line (split /\n/, slurp($file))
    {
        if ($line =~ /^\s*\.equ\s+(\w+)\s*,\s*(.*?)\s*$/) { $equ{$scope}{$1} = $2; next }
        next if $line =~ /^\s*\.include/;
        if ($line =~ /^(\w+)::?\s*(@.*)?$/)
        {
            $cur = $1;
            if (exists $label{$cur}) { $label{$cur}{dup}++ }
            else { $label{$cur} = { file => $file, scope => $scope, lines => [] } }
            next;
        }
        push @{$label{$cur}{lines}}, $line if defined $cur;
    }
}
for my $f (glob("$frlg/data/scripts/*.inc"), glob("$frlg/data/text/*.inc")) { index_script_file($f, 'shared') }
index_script_file("$frlg/data/event_scripts.s", 'shared');
my %frMapNames;
for my $d (glob("$frlg/data/maps/*")) { my $m = basename($d); $frMapNames{$m} = 1; index_script_file("$d/scripts.inc", "map:$m"); index_script_file("$d/text.inc", "map:$m") }

# ---------------------------------------------------------------------------------------------
# The map list
# ---------------------------------------------------------------------------------------------
my @maps;
for (split /\n/, slurp($mapsFile)) { s/#.*//; s/^\s+|\s+$//g; next unless length; die "map $_ is not in pokefirered\n" unless $frMapNames{$_}; push @maps, $_ }
my %inList = map { $_ => 1 } @maps;
section("PS-100 event measurements: " . scalar(@maps) . " maps from $mapsFile");

# ---------------------------------------------------------------------------------------------
# 1. Events
# ---------------------------------------------------------------------------------------------
my (%kind, %gfxCount, %gfxMaps, %movementUsed, %trainerTypeUsed, %sightUsed, %bgKind, %coordVars, %hiddenQty, %hiddenUnderfoot);
my ($objTotal, $coordTotal, $bgTotal, $clones, $noLocalId, $objFlags, $objFlag0, $objScripts0) = (0) x 8;
my (%objFlagsUsed, %hiddenFlags, %itemBallItems, %hiddenItems, %localIds, %eventScripts);
my (%facingDirs, %coordVarValues);
my %perMapCounts;
for my $m (@maps)
{
    my $j = rjson("$frlg/data/maps/$m/map.json");
    for my $o (@{$j->{object_events} || []})
    {
        $objTotal++;
        $perMapCounts{$m}{objects}++;
        if (($o->{type} // 'object') eq 'clone') { $clones++; $kind{clone}++; next }
        my $g = $o->{graphics_id} // '?';
        $gfxCount{$g}++; $gfxMaps{$g}{$m} = 1;
        my $k = ($o->{trainer_type} // '') ne 'TRAINER_TYPE_NONE' ? 'trainer'
              : $g eq 'OBJ_EVENT_GFX_ITEM_BALL' ? 'item ball'
              : $g eq 'OBJ_EVENT_GFX_ROCK_SMASH_ROCK' ? 'rock'
              : $g eq 'OBJ_EVENT_GFX_CUT_TREE' ? 'tree'
              : $g eq 'OBJ_EVENT_GFX_PUSHABLE_BOULDER' ? 'boulder'
              : $g eq '0' ? 'invisible (graphics_id 0)'
              : ($o->{script} // '0x0') eq '0x0' ? 'NPC without script' : 'NPC';
        $kind{$k}++;
        $movementUsed{$o->{movement_type} // '?'}++;
        $trainerTypeUsed{$o->{trainer_type} // '?'}++;
        $sightUsed{$o->{trainer_sight_or_berry_tree_id} // '?'}++ if $k eq 'trainer';
        $localIds{$o->{local_id}}++ if defined $o->{local_id};
        $noLocalId++ unless defined $o->{local_id};
        my $flag = $o->{flag} // '0';
        if ($flag eq '0') { $objFlag0++ } else { $objFlagsUsed{$flag}++ }
        $eventScripts{$o->{script}} = $m if ($o->{script} // '0x0') ne '0x0';
        $objScripts0++ if ($o->{script} // '0x0') eq '0x0';
    }
    for my $c (@{$j->{coord_events} || []})
    {
        $coordTotal++;
        $perMapCounts{$m}{coord}++;
        $coordVars{$c->{var} // '?'}++;
        $eventScripts{$c->{script}} = $m if ($c->{script} // '0x0') ne '0x0';
    }
    for my $b (@{$j->{bg_events} || []})
    {
        $bgTotal++;
        $perMapCounts{$m}{bg}++;
        $bgKind{$b->{type}}++;
        if ($b->{type} eq 'sign') { $facingDirs{$b->{player_facing_dir}}++; $eventScripts{$b->{script}} = $m if ($b->{script} // '0x0') ne '0x0' }
        if ($b->{type} eq 'hidden_item')
        {
            $hiddenQty{$b->{quantity} // 1}++;
            $hiddenUnderfoot{$b->{underfoot} ? 'underfoot' : 'normal'}++;
            $hiddenFlags{$b->{flag}}++;
            $hiddenItems{$b->{item}}++;
        }
    }
}
section("1. Object events ($objTotal), coord events ($coordTotal), bg events ($bgTotal)");
print "  object events by kind:\n";
printf "    %-28s %5d\n", $_, $kind{$_} for sort { $kind{$b} <=> $kind{$a} } keys %kind;
print "  object events without a local_id name: $noLocalId; with flag 0 (always shown): $objFlag0; with script 0x0: $objScripts0; distinct hide flags: " . scalar(keys %objFlagsUsed) . "\n";
print "  bg events by type: " . join(", ", map { "$_ $bgKind{$_}" } sort keys %bgKind) . "\n";
print "  sign facing dirs: " . join(", ", map { "$_ $facingDirs{$_}" } sort keys %facingDirs) . "\n";
print "  hidden item quantities: " . join(", ", map { "x$_: $hiddenQty{$_}" } sort { $a <=> $b } keys %hiddenQty) . "; " . join(", ", map { "$_ $hiddenUnderfoot{$_}" } sort keys %hiddenUnderfoot) . "\n";
print "  hidden items by item: " . join(", ", map { "$_ $hiddenItems{$_}" } sort { $hiddenItems{$b} <=> $hiddenItems{$a} } keys %hiddenItems) . "\n";
print "  coord event trigger vars: " . join(", ", map { "$_ $coordVars{$_}" } sort { $coordVars{$b} <=> $coordVars{$a} } keys %coordVars) . "\n";
print "  trainer_type: " . join(", ", map { "$_ $trainerTypeUsed{$_}" } sort keys %trainerTypeUsed) . "; trainer sight ranges: " . join(", ", map { "$_: $sightUsed{$_}" } sort { $a <=> $b } keys %sightUsed) . "\n";
print "  movement types used: " . join(", ", map { "$_ $movementUsed{$_}" } sort { $movementUsed{$b} <=> $movementUsed{$a} } keys %movementUsed) . "\n";
listing("movement types missing here", grep { !$our{movement}{$_} } sort keys %movementUsed);

# sprites
my @usedGfx = sort { $gfxCount{$b} <=> $gfxCount{$a} || $a cmp $b } keys %gfxCount;
my @sameName = grep { $our{gfx}{$_} } @usedGfx;
my @frOnly = grep { !$our{gfx}{$_} && $_ ne '0' } @usedGfx;
print "\n  distinct sprites used: " . scalar(@usedGfx) . "; with the same OBJ_EVENT_GFX_ name here: " . scalar(@sameName) . "; FRLG-only names: " . scalar(@frOnly) . "\n";
print "  same name here: " . join(", ", map { "$_ $gfxCount{$_}" } @sameName) . "\n";
print "  FRLG-only: " . join(", ", map { "$_ $gfxCount{$_}" } @frOnly) . "\n";
my $frlgObjectsOnly = 0; $frlgObjectsOnly += $gfxCount{$_} for @frOnly;
print "  object events that use a FRLG-only sprite: $frlgObjectsOnly of " . ($objTotal - $clones) . "\n";
my $frGfxDefined = scalar(grep { !/^OBJ_EVENT_GFX_VAR/ } keys %{$fr{gfx}});
my $ourGfxDefined = scalar(grep { !/^OBJ_EVENT_GFX_VAR/ } keys %{$our{gfx}});
print "  OBJ_EVENT_GFX_ names defined: pokefirered $frGfxDefined, here $ourGfxDefined (NUM_OBJ_EVENT_GFX 240, no free id below OBJ_EVENT_GFX_VARS)\n";

# ---------------------------------------------------------------------------------------------
# 2. Scripts: closure of everything the maps reference
# ---------------------------------------------------------------------------------------------
my %needed;      # label => 1
my @queue;
for my $m (@maps)
{
    for my $l (keys %label) { next unless $label{$l}{scope} eq "map:$m"; $needed{$l} = 1; push @queue, $l }
}
my $mapLabels = scalar(keys %needed);
my (%sharedNeeded, %otherMapNeeded, %missingLabels, %refsToUnknown);
while (my $l = shift @queue)
{
    for my $line (@{$label{$l}{lines}})
    {
        next if $line =~ /^\s*\.string/;
        (my $code = $line) =~ s/@.*//;
        for my $tok ($code =~ /\b([A-Za-z_]\w*)\b/g)
        {
            next if $needed{$tok};
            if (exists $label{$tok})
            {
                $needed{$tok} = 1; push @queue, $tok;
                if ($label{$tok}{scope} eq 'shared') { $sharedNeeded{$tok} = $l }
                elsif ($label{$tok}{scope} =~ /^map:(\w+)$/ && !$inList{$1}) { $otherMapNeeded{$tok} = "$1 (from $l)" }
            }
        }
    }
}
# labels referenced by map.json events but undefined
for my $s (keys %eventScripts) { $missingLabels{$s} = $eventScripts{$s} unless exists $label{$s} }
section("2. Scripts: $mapLabels labels in the maps' scripts.inc / text.inc; closure pulls " . scalar(keys %sharedNeeded) . " shared labels and " . scalar(keys %otherMapNeeded) . " labels of maps outside the list");
my ($scriptLines, $textLines) = (0, 0);
for my $l (keys %needed) { for (@{$label{$l}{lines}}) { if (/^\s*\.string/) { $textLines++ } elsif (/\S/ && !/^\s*@/) { $scriptLines++ } } }
print "  lines: $scriptLines script lines, $textLines .string lines (closure, comments and blanks excluded)\n";
my %sharedByFile;
for my $l (keys %sharedNeeded) { (my $f = $label{$l}{file}) =~ s/^\Q$frlg\E\///; $sharedByFile{$f}++ }
print "  shared labels by file: " . join(", ", map { "$_ $sharedByFile{$_}" } sort { $sharedByFile{$b} <=> $sharedByFile{$a} } keys %sharedByFile) . "\n";
my @collide = sort grep { $ourLabels{$_} } keys %sharedNeeded;
listing("shared labels whose name is also a label here (same name, meaning to check)", @collide);
my @collideMap = sort grep { $ourLabels{$_} } grep { $label{$_}{scope} ne 'shared' } keys %needed;
listing("map labels whose name is also a label here", @collideMap);
listing("labels of maps outside the list that the maps reference", map { "$_ <- $otherMapNeeded{$_}" } sort keys %otherMapNeeded);
listing("event scripts referenced by map.json but defined nowhere", map { "$_ ($missingLabels{$_})" } sort keys %missingLabels);
my @dups = sort grep { $label{$_}{dup} } keys %needed;
listing("labels defined more than once in pokefirered", @dups);
my %equsUsed;
for my $m (@maps) { $equsUsed{$_} = $equ{"map:$m"}{$_} for keys %{$equ{"map:$m"} || {}} }
$equsUsed{$_} = $equ{shared}{$_} for keys %{$equ{shared} || {}};
listing(".equ aliases in the maps' scripts (+ shared)", map { "$_=$equsUsed{$_}" } sort keys %equsUsed);
listing(".equ aliases that collide with a constant or label here", grep { $ourConstants{$_} || $ourLabels{$_} } sort keys %equsUsed);

# ---------------------------------------------------------------------------------------------
# 3. Identifiers referenced by the closure
# ---------------------------------------------------------------------------------------------
my (%flagsRef, %varsRef, %trainersRef, %itemsRef, %speciesRef, %songsRef, %specialsRef, %macrosRef, %capsRef, %placeholders, %gfxInScripts, %mapsRef, %localIdsRef);
my %specialsBy; # special => { map => n }
for my $l (keys %needed)
{
    my $scope = $label{$l}{scope};
    for my $line (@{$label{$l}{lines}})
    {
        if ($line =~ /^\s*\.string\s+"(.*)"/)
        {
            $placeholders{$_}++ for $1 =~ /\{([A-Z][A-Z0-9_]*)\}/g;
            next;
        }
        (my $code = $line) =~ s/@.*//;
        next unless $code =~ /\S/;
        if ($code =~ /^\s*(\.?\w+)/) { my $mac = $1; $macrosRef{$mac}++ unless $mac =~ /^\./ }
        if ($code =~ /^\s*special\s+(\w+)/ || $code =~ /^\s*specialvar\s+\w+\s*,\s*(\w+)/) { $specialsRef{$1}++; $specialsBy{$1}{$scope}++ }
        for my $tok ($code =~ /\b([A-Z][A-Z0-9_]*)\b/g)
        {
            if    ($tok =~ /^FLAG_/) { $flagsRef{$tok}++ }
            elsif ($tok =~ /^VAR_/) { $varsRef{$tok}++ }
            elsif ($tok =~ /^TRAINER_/ && $fr{trainers}{$tok}) { $trainersRef{$tok}++ }
            elsif ($tok =~ /^ITEM_/) { $itemsRef{$tok}++ }
            elsif ($tok =~ /^SPECIES_/) { $speciesRef{$tok}++ }
            elsif ($tok =~ /^(MUS|SE)_/) { $songsRef{$tok}++ }
            elsif ($tok =~ /^OBJ_EVENT_GFX_/) { $gfxInScripts{$tok}++ }
            elsif ($tok =~ /^MAP_/) { $mapsRef{$tok}++ }
            elsif ($tok =~ /^LOCALID_/) { $localIdsRef{$tok}++ }
            else { $capsRef{$tok}++ }
        }
    }
}
# object/bg/coord event fields count too
$flagsRef{$_} += $objFlagsUsed{$_} for keys %objFlagsUsed;
$flagsRef{$_} += $hiddenFlags{$_} for keys %hiddenFlags;
$varsRef{$_} += $coordVars{$_} for keys %coordVars;
$itemsRef{$_} += $hiddenItems{$_} for keys %hiddenItems;
$localIdsRef{$_} += $localIds{$_} for keys %localIds;

section("3. Flags and vars");
my @flags = sort keys %flagsRef;
my @flagsEngine = grep { /^FLAG_TEMP_/ } @flags;
my @flagsSame = grep { !/^FLAG_TEMP_/ && $our{flags}{$_} } @flags;
my @flagsNew = grep { !/^FLAG_TEMP_/ && !$our{flags}{$_} } @flags;
my @flagsUndef = grep { !$fr{flags}{$_} } @flags;
print "  distinct flags: " . scalar(@flags) . " (FRLG defines " . scalar(keys %{$fr{flags}}) . ")\n";
listing("engine-shared (FLAG_TEMP_*)", @flagsEngine);
listing("same name exists here (meaning to decide: share or FLAG_KANTO_)", @flagsSame);
my %flagGroups;
for (@flagsNew) { my $g = /^FLAG_HIDDEN_ITEM_/ ? 'FLAG_HIDDEN_ITEM_*' : /^FLAG_HIDE_/ ? 'FLAG_HIDE_*' : /^FLAG_WORLD_MAP_/ ? 'FLAG_WORLD_MAP_*' : /^FLAG_SYS_/ ? 'FLAG_SYS_*' : /^FLAG_GOT_|^FLAG_RECEIVED_/ ? 'FLAG_GOT_* / RECEIVED_*' : /^FLAG_DEFEATED_/ ? 'FLAG_DEFEATED_*' : /^FLAG_ITEM_/ ? 'FLAG_ITEM_* (item balls)' : 'other story flags'; $flagGroups{$g}++ }
print "  FRLG-only names (-> FLAG_KANTO_*): " . scalar(@flagsNew) . " = " . join(", ", map { "$_ $flagGroups{$_}" } sort keys %flagGroups) . "\n";
listing("FRLG-only FLAG_SYS_*", grep { /^FLAG_SYS_/ } @flagsNew);
listing("referenced but not defined in pokefirered", @flagsUndef);
my @vars = sort keys %varsRef;
my @varsEngine = grep { /^VAR_TEMP_|^VAR_0x80|^VAR_RESULT$|^VAR_FACING$|^VAR_LAST_TALKED$|^VAR_OBJ_GFX_ID_/ } @vars;
my @varsSame = grep { !/^VAR_TEMP_|^VAR_0x80|^VAR_RESULT$|^VAR_FACING$|^VAR_LAST_TALKED$|^VAR_OBJ_GFX_ID_/ && $our{vars}{$_} } @vars;
my @varsNew = grep { !$our{vars}{$_} } @vars;
print "\n  distinct vars: " . scalar(@vars) . " (FRLG defines " . scalar(keys %{$fr{vars}}) . ")\n";
listing("engine-shared (VAR_TEMP_*, VAR_0x800x, VAR_RESULT, VAR_FACING, VAR_LAST_TALKED, VAR_OBJ_GFX_ID_*)", @varsEngine);
listing("same name exists here (meaning to decide)", @varsSame);
my ($mapScene, $otherVars) = (0, 0);
for (@varsNew) { if (/^VAR_MAP_SCENE_/) { $mapScene++ } else { $otherVars++ } }
print "  FRLG-only names (-> VAR_KANTO_*): " . scalar(@varsNew) . " ($mapScene VAR_MAP_SCENE_*, $otherVars other)\n";
listing("FRLG-only vars other than VAR_MAP_SCENE_*", grep { !/^VAR_MAP_SCENE_/ } @varsNew);
listing("referenced but not defined in pokefirered", grep { !$fr{vars}{$_} } @vars);

section("4. Specials called (" . scalar(keys %specialsRef) . " distinct)");
my @spSame = sort grep { $ourSpecials->{$_} } keys %specialsRef;
my @spMissing = sort grep { !$ourSpecials->{$_} } keys %specialsRef;
listing("same name here", map { "$_ $specialsRef{$_}" } @spSame);
listing("not here (twin / stub / port, see specials.tsv)", map { "$_ $specialsRef{$_}" } @spMissing);
listing("not even in pokefirered's specials.inc", grep { !$frSpecials->{$_} } sort keys %specialsRef);
if ($verbose) { for my $s (@spMissing) { print "    $s: " . join(", ", map { "$_ $specialsBy{$s}{$_}" } sort keys %{$specialsBy{$s}}) . "\n" } }

section("5. Macros used (" . scalar(keys %macrosRef) . " distinct first tokens)");
my @macMissing = sort grep { !$ourMacros->{$_} && !$ourMovementMacros{$_} } keys %macrosRef;
listing("not a macro here (FRLG-only macro, or a label used as a line start)", map { "$_ $macrosRef{$_}" } @macMissing);
listing("FRLG-only event macros (all 12) used", map { "$_ $macrosRef{$_}" } grep { $macrosRef{$_} } sort grep { !$ourMacros->{$_} } keys %$frMacros);

section("6. Trainers referenced (" . scalar(keys %trainersRef) . " distinct ids; pokefirered defines " . scalar(keys %{$fr{trainers}}) . ")");
my (%classUse, %picUse, %musicUse, %classMissing, %picMissing, %musicMissing);
my $doubles = 0;
for my $t (sort keys %trainersRef)
{
    my $e = $frTrainer{$t} or do { print "  $t has no gTrainers entry\n"; next };
    $classUse{$e->{class} // '?'}++; $picUse{$e->{pic} // '?'}++;
    (my $mus = $e->{music} // '?') =~ s/F_TRAINER_FEMALE\s*\|\s*//; $musicUse{$mus}++;
    $classMissing{$e->{class}}++ if $e->{class} && !$our{classes}{$e->{class}};
    $picMissing{$e->{pic}}++ if $e->{pic} && !$our{pics}{$e->{pic}};
    $musicMissing{$mus}++ unless $our{music}{$mus};
    $doubles++ if $e->{double};
}
print "  double battles: $doubles\n";
print "  classes used: " . join(", ", map { "$_ $classUse{$_}" } sort { $classUse{$b} <=> $classUse{$a} } keys %classUse) . "\n";
listing("classes with no TRAINER_CLASS_ of the same name here (trainers using them)", map { "$_ $classMissing{$_}" } sort keys %classMissing);
listing("pics with no TRAINER_PIC_ of the same name here", map { "$_ $picMissing{$_}" } sort keys %picMissing);
listing("encounter music missing here", sort keys %musicMissing);
listing("trainer ids whose name is also a TRAINER_ here", grep { $our{trainers}{$_} } sort keys %trainersRef);
listing("FRLG trainer classes with no twin here (all)", sort grep { !$our{classes}{$_} } keys %frClasses);

section("7. Items, species, songs, maps, local ids, other constants");
my @itemsMissing = sort grep { !$our{items}{$_} } keys %itemsRef;
my @tmhm = grep { /^ITEM_(TM|HM)\d\d_/ } @itemsMissing;
my @itemsOther = grep { !/^ITEM_(TM|HM)\d\d_/ } @itemsMissing;
print "  distinct items: " . scalar(keys %itemsRef) . "; TM/HM names that only differ in the suffix: " . scalar(@tmhm) . "\n";
listing("items with no ITEM_ here (other than TM/HM names)", @itemsOther);
listing("species not here", sort grep { !$our{species}{$_} } keys %speciesRef);
my @songsMissing;
for (sort keys %songsRef) { (my $rg = $_) =~ s/^MUS_/MUS_RG_/; push @songsMissing, $_ unless $our{songs}{$rg} || $our{songs}{$_} }
print "  distinct songs / sounds: " . scalar(keys %songsRef) . "\n";
listing("songs with neither MUS_RG_ twin nor the same name here", @songsMissing);
my @mapsOutside = sort grep { my $id = $_; !grep { "MAP_" . uc($_) =~ /^\Q$id\E$/ } () } keys %mapsRef;
print "  distinct MAP_ constants referenced: " . scalar(keys %mapsRef) . " (" . scalar(grep { /^MAP_(DYNAMIC|NONE|UNDEFINED)$/ } keys %mapsRef) . " special)\n";
my %inListIds;
for my $m (@maps) { my $j = rjson("$frlg/data/maps/$m/map.json"); $inListIds{$j->{id}} = 1 }
listing("MAP_ constants of maps outside the list (warp targets in scripts)", grep { !$inListIds{$_} && !/^MAP_(DYNAMIC|NONE|UNDEFINED)$/ } sort keys %mapsRef);
print "  distinct LOCALID_ names: " . scalar(keys %localIdsRef) . "; in scripts: " . scalar(grep { !$localIds{$_} } keys %localIdsRef) . " not defined by the maps' object events (LOCALID_PLAYER etc.)\n";
listing("LOCALID_ in scripts not defined by these maps", grep { !$localIds{$_} } sort keys %localIdsRef);
listing("OBJ_EVENT_GFX_ in scripts (setvar VAR_OBJ_GFX_ID_*)", map { "$_ $gfxInScripts{$_}" } sort keys %gfxInScripts);
my @capsUnknown = sort grep { !$ourConstants{$_} && !$ourCharmap{$_} && !$equsUsed{$_} && !$our{trainers}{$_} && !$ourLabels{$_} && !$label{$_} && !/^(TRUE|FALSE|YES|NO|NULL)$/ } keys %capsRef;
listing("other CAPS identifiers not defined here (constants to map or shim)", map { "$_ $capsRef{$_}" } @capsUnknown);

section("8. Text placeholders (" . scalar(keys %placeholders) . " distinct) and charmap");
print "  used: " . join(", ", map { "{$_} $placeholders{$_}" } sort { $placeholders{$b} <=> $placeholders{$a} } keys %placeholders) . "\n";
listing("not in our charmap", map { "{$_} (FRLG: $frCharmap{$_})" } sort grep { !$ourCharmap{$_} } keys %placeholders);
my @diffCode = sort grep { $ourCharmap{$_} } grep { my $n = $_; my ($o) = grep { /^\Q$n\E\s*=/ } split /\n/, slurp("$repo/charmap.txt"); $o && $o !~ /=\s*\Q$frCharmap{$n}\E\s*$/ } keys %placeholders;
listing("same name, different code (music ids etc.)", map { "{$_} FRLG $frCharmap{$_}" } @diffCode);

section("9. Per-map counts (objects / coord / bg)");
for my $m (@maps) { printf "  %-45s %4d %4d %4d\n", $m, $perMapCounts{$m}{objects} // 0, $perMapCounts{$m}{coord} // 0, $perMapCounts{$m}{bg} // 0 }
