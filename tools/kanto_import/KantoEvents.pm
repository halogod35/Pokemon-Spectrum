# tools/kanto_import/KantoEvents.pm -- PS-100: the story import (object / bg / coord events, map scripts, text,
# shared scripts, sprites, trainers, flags and vars) of pokefirered's Kanto maps, driven by kanto_import.pl
# when it is given --events <map list>. Everything here is a mechanical translation of pokefirered's data
# through the tables next to this file (specials.tsv, shared_ids.tsv, trainers.tsv, script_constants.tsv,
# text.tsv) and the stable id allocations it records (kanto_ids.tsv, kanto_sprites.tsv). The pokefirered
# files are data: nothing is executed from that checkout. Perl core modules only.
#
# Naming: every imported label gets the prefix "Kanto" (PalletTown_EventScript_X -> KantoPalletTown_EventScript_X,
# EventScript_Return -> KantoEventScript_Return), flags FLAG_KANTO_*, vars VAR_KANTO_*, trainers TRAINER_KANTO_*,
# local ids LOCALID_KANTO_*, sprites OBJ_EVENT_GFX_KANTO_*, metatile labels METATILE_Kanto*. Engine-shared ids
# (FLAG_TEMP_*, VAR_TEMP_*, VAR_0x800x, VAR_RESULT, ...) and the rows of shared_ids.tsv keep Emerald's name.
package KantoEvents;
use strict;
use warnings;
use JSON::PP;
use File::Basename qw(dirname basename);

my %C;                    # context from the driver (see init)
my ($frlg, $repo, $toolDir);
my $PFX = 'Kanto';        # label / symbol prefix
my $UPFX = 'KANTO';       # constant prefix

sub err  { $C{err}->(@_) }
sub note { $C{note}->(@_) }
sub info { $C{info}->(@_) }
sub slurp { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; $d =~ s/\r\n/\n/g; return $d }
sub slurp_raw { my $f = shift; open my $h, '<', $f or die "cannot read $f: $!\n"; binmode $h; local $/; my $d = <$h>; close $h; return $d }
sub defines { my ($file, $prefix) = @_; my %d; for (split /\n/, slurp($file)) { $d{$1} = 1 if /^#define\s+($prefix\w*)\b/ } return \%d } # presence (a value of 0 would be false)
sub define_values { my ($file, $prefix) = @_; my %d; for (split /\n/, slurp($file)) { $d{$1} = $2 if /^#define\s+($prefix\w*)\s+(\d+)\s*(\/\/.*)?$/ } return \%d } # numeric values only
sub read_tsv
{
    my $f = shift; my %t; my @order;
    return (\%t, \@order) unless -f $f;
    for (split /\n/, slurp($f))
    {
        next if /^\s*(#|$)/;
        my ($from, $to, $noteText) = split /\t/;
        $t{$from} = { to => $to // '', note => $noteText // '' };
        push @order, $from;
    }
    return (\%t, \@order);
}
sub jstr { my $s = shift; $s =~ s/(["\\])/\\$1/g; return "\"$s\"" }

# ---------------------------------------------------------------------------------------------
# Tables and definitions
# ---------------------------------------------------------------------------------------------
my ($specialsTable, $sharedIds, $trainersTable, $scriptConsts, $textTable, $constTable, $sharedLabels);
my %extraRoots;                     # FRLG map => [labels] (extra_roots.tsv: scripts pokefirered's C code starts)
my (%idAlloc, @idAllocOrder);       # FLAG_KANTO_* / VAR_KANTO_* -> offset (kanto_ids.tsv)
my (%spriteAlloc, @spriteAllocOrder); # OBJ_EVENT_GFX_KANTO_* -> index (kanto_sprites.tsv)
my %fr;   # pokefirered definitions
my %our;  # this repository's definitions
my (%frTrainer, %frParty, %frInfo, %frPicTable, %frPicPath, %frGfxInfo, %frPngOpts, %frMetatile);
my %frSpecials; my %ourSpecials; my %ourMacros; my %ourMovementMacros; my %ourLabels; my %ourConstants; my %ourCharmapNames; my %ourCharmapChars;

# pokefirered script sources: label -> { file, scope, lines }, macros -> pseudo labels "__macro__<name>"
my (%label, %fileEqus, %fileOrder);
sub index_script_file
{
    my ($file, $scope) = @_;
    return unless -f $file;
    my ($cur, $inMacro, $n) = (undef, undef, 0);
    for my $line (split /\n/, slurp($file))
    {
        $n++;
        if ($inMacro) { push @{$label{$inMacro}{lines}}, $line; $inMacro = undef if $line =~ /^\s*\.endm\b/; next }
        if ($line =~ /^\s*\.macro\s+(\w+)/) { $inMacro = "__macro__$1"; $label{$inMacro} = { file => $file, scope => $scope, lines => [$line], line => $n, macro => $1 }; $cur = undef; next }
        if ($line =~ /^\s*\.equ\s+(\w+)\s*,\s*(.*?)\s*$/) { push @{$fileEqus{$file}}, [$1, $2]; next }
        next if $line =~ /^\s*\.include/;
        if ($line =~ /^(\w+)::?\s*(@.*)?$/)
        {
            $cur = $1;
            if (exists $label{$cur}) { $label{$cur}{dup}++ } else { $label{$cur} = { file => $file, scope => $scope, lines => [], line => $n } }
            next;
        }
        push @{$label{$cur}{lines}}, $line if defined $cur;
    }
}

sub init
{
    my $ctx = shift; %C = %$ctx;
    ($frlg, $repo, $toolDir) = ($C{frlg}, $C{repo}, $C{toolDir});
    ($specialsTable) = read_tsv("$toolDir/specials.tsv");
    ($sharedIds)     = read_tsv("$toolDir/shared_ids.tsv");
    ($trainersTable) = read_tsv("$toolDir/trainers.tsv");
    ($scriptConsts)  = read_tsv("$toolDir/script_constants.tsv");
    ($textTable)     = read_tsv("$toolDir/text.tsv");
    ($constTable)    = read_tsv("$toolDir/constants.tsv");
    for (split /\n/, slurp("$toolDir/extra_roots.tsv")) { next if /^\s*(#|$)/; my ($m, $l) = split /\t/; push @{$extraRoots{$m}}, $l }
    {
        my ($t, $o) = read_tsv("$toolDir/kanto_ids.tsv");
        for (@$o) { $idAlloc{$_} = hex($t->{$_}{to}); push @idAllocOrder, $_ }
        ($t, $o) = read_tsv("$toolDir/kanto_sprites.tsv");
        for (@$o) { $spriteAlloc{$_} = $t->{$_}{to} + 0; push @spriteAllocOrder, $_ }
    }
    %fr = (
        flags    => defines("$frlg/include/constants/flags.h", 'FLAG_'),
        vars     => defines("$frlg/include/constants/vars.h", 'VAR_'),
        items    => defines("$frlg/include/constants/items.h", 'ITEM_'),
        gfx      => defines("$frlg/include/constants/event_objects.h", 'OBJ_EVENT_GFX_'),
        trainers => defines("$frlg/include/constants/opponents.h", 'TRAINER_'),
    );
    $fr{trainers} = define_values("$frlg/include/constants/opponents.h", 'TRAINER_'); # name => id (the numeric defines only, not TRAINERS_COUNT etc.)
    delete $fr{trainers}{$_} for grep { /_COUNT$/ } keys %{$fr{trainers}};
    ($sharedLabels) = read_tsv("$toolDir/shared_labels.tsv");
    %our = (
        flags    => defines("$repo/include/constants/flags.h", 'FLAG_'),
        vars     => defines("$repo/include/constants/vars.h", 'VAR_'),
        items    => defines("$repo/include/constants/items.h", 'ITEM_'),
        gfx      => defines("$repo/include/constants/event_objects.h", 'OBJ_EVENT_GFX_'),
        movement => defines("$repo/include/constants/event_object_movement.h", 'MOVEMENT_TYPE_'),
        species  => defines("$repo/include/constants/species.h", 'SPECIES_'),
        moves    => defines("$repo/include/constants/moves.h", 'MOVE_'),
        songs    => defines("$repo/include/constants/songs.h", '(?:MUS|SE)_'),
        classes  => defines("$repo/include/constants/trainers.h", 'TRAINER_CLASS_'),
        pics     => defines("$repo/include/constants/trainers.h", 'TRAINER_PIC_'),
        tutors   => defines("$repo/include/constants/party_menu.h", 'TUTOR_MOVE_'),
    );
    for (split /\n/, slurp("$frlg/data/specials.inc")) { $frSpecials{$1} = 1 if /def_special\s+(\w+)/ }
    for (split /\n/, slurp("$repo/data/specials.inc")) { $ourSpecials{$1} = 1 if /def_special\s+(\w+)/ }
    for (split /\n/, slurp("$repo/asm/macros/event.inc")) { $ourMacros{$1} = 1 if /^\s*\.macro\s+(\w+)/ }
    for (split /\n/, slurp("$repo/asm/macros/movement.inc")) { $ourMovementMacros{$1} = 1 if /^\s*create_movement_action\s+(\w+)/ || /^\s*\.macro\s+(\w+)/ }
    for (split /\n/, slurp("$repo/asm/macros/map.inc")) { $ourMacros{$1} = 1 if /^\s*\.macro\s+(\w+)/ } # map_script, map_script_2
    for my $f ("$repo/data/event_scripts.s", glob("$repo/data/scripts/*.inc"), glob("$repo/data/text/*.inc"), glob("$repo/data/maps/*/scripts.inc"))
    {
        next if $f =~ /kanto_shared\.inc$/ || $f =~ m{/data/maps/Kanto};
        for (split /\n/, slurp($f)) { $ourLabels{$1} = 1 if /^(\w+)::?\s*(\/\*.*|@.*)?$/ }
    }
    for my $f (glob("$repo/include/constants/*.h"), glob("$repo/include/constants/*/*.h"), "$repo/asm/macros/event.inc", "$repo/asm/macros/movement.inc", "$repo/asm/macros.inc")
    {
        for (split /\n/, slurp($f)) { $ourConstants{$1} = 1 if /^\s*#define\s+([A-Za-z_]\w*)/ || /^\s*([A-Z][A-Z0-9_]*)\s*=/ || /^\s*\.set\s+([A-Z][A-Z0-9_]*)/ || /^\s+([A-Z][A-Z0-9_]+),?\s*(\/\/.*)?$/ }
    }
    for (split /\n/, slurp("$repo/charmap.txt"))
    {
        if (/^'(.*?)'\s*=/) { my $ch = $1; $ch =~ s/^\\'$/'/; utf8::decode($ch); $ourCharmapChars{$ch} = 1 } # code points: the files are UTF-8 bytes
        elsif (/^([A-Za-z_][A-Za-z0-9_]*)\s*=/) { $ourCharmapNames{$1} = 1 }
    }
    for (split /\n/, slurp("$frlg/include/constants/metatile_labels.h")) { $frMetatile{$1} = $2 if /^#define\s+(METATILE_\w+)\s+(0x[0-9A-Fa-f]+|\d+)/ }

    # pokefirered trainers and parties (raw text per entry)
    {
        my $t = slurp("$frlg/src/data/trainers.h");
        while ($t =~ /(\[(TRAINER_\w+)\]\s*=\s*\{.*?\n\s*\},?)/sg) { $frTrainer{$2} = $1 }
        my $p = slurp("$frlg/src/data/trainer_parties.h");
        while ($p =~ /(static const struct (TrainerMon\w+) (sParty_\w+)\[\] = \{.*?\n\};)/sg) { $frParty{$3} = { text => $1, kind => $2 } }
    }
    # pokefirered sprites: gfx id -> info symbol -> fields, pic tables, pic paths, gbagfx options
    {
        my $ptr = slurp("$frlg/src/data/object_events/object_event_graphics_info_pointers.h");
        while ($ptr =~ /\[(OBJ_EVENT_GFX_\w+)\]\s*=\s*&(gObjectEventGraphicsInfo_\w+)/g) { $frGfxInfo{$1} = $2 }
        my $inf = slurp("$frlg/src/data/object_events/object_event_graphics_info.h");
        while ($inf =~ /const struct ObjectEventGraphicsInfo (gObjectEventGraphicsInfo_\w+) = \{(.*?)\};/sg)
        {
            my ($sym, $body) = ($1, $2); my %f;
            while ($body =~ /\.(\w+)\s*=\s*([^,\n]+?)\s*,?\s*(?:\n|$)/g) { my ($k, $v) = ($1, $2); $v =~ s/,\s*$//; $f{$k} = $v }
            $frInfo{$sym} = \%f;
        }
        my $pic = slurp("$frlg/src/data/object_events/object_event_pic_tables.h");
        while ($pic =~ /static const struct SpriteFrameImage (sPicTable_\w+)\[\] = \{(.*?)\};/sg)
        {
            my ($sym, $body) = ($1, $2);
            $frPicTable{$sym} = [ grep { /\S/ } map { s/^\s+|\s+$//gr } split /\n/, $body ];
        }
        my $g = slurp("$frlg/src/data/object_events/object_event_graphics.h");
        while ($g =~ /const u16 (gObjectEventPic_\w+)\[\] = INCBIN_U16\("([^"]+)\.4bpp"\)/g) { $frPicPath{$1} = $2 }
        my $rules = slurp("$frlg/spritesheet_rules.mk");
        while ($rules =~ /^\$\(OBJEVENTGFXDIR\)\/(\S+)\.4bpp: %\.4bpp: %\.png\n\t\$\(GFX\) \$< \$@ (.*?)\s*$/mg) { $frPngOpts{"graphics/object_events/pics/$1"} = $2 }
    }
    # pokefirered script sources
    for my $f (glob("$frlg/data/scripts/*.inc"), glob("$frlg/data/text/*.inc"), "$frlg/data/event_scripts.s") { index_script_file($f, 'shared') }
    for my $d (glob("$frlg/data/maps/*")) { my $m = basename($d); index_script_file("$d/scripts.inc", "map:$m"); index_script_file("$d/text.inc", "map:$m") }
    my $i = 0;
    for my $f (glob("$frlg/data/scripts/*.inc"), glob("$frlg/data/text/*.inc"), "$frlg/data/event_scripts.s") { $fileOrder{$f} = $i++ }
    # table sanity
    for my $k (sort keys %$trainersTable) { my $to = $trainersTable->{$k}{to}; next if $k eq 'rival_name'; err("trainers.tsv: $k -> $to is not a TRAINER_CLASS_ / TRAINER_PIC_ here") unless $our{classes}{$to} || $our{pics}{$to} }
    for my $k (sort keys %$sharedIds) { my $to = $sharedIds->{$k}{to}; err("shared_ids.tsv: $k -> $to is neither an Emerald flag / var nor a KANTO name") unless $our{flags}{$to} || $our{vars}{$to} || $to =~ /^(FLAG|VAR)_${UPFX}_/ }
    for my $k (sort keys %$sharedLabels) { my $to = $sharedLabels->{$k}{to}; err("shared_labels.tsv: $k -> $to is not a script label here") unless $ourLabels{$to}; err("shared_labels.tsv: $k is not a pokefirered label") unless exists $label{$k} }
}

# ---------------------------------------------------------------------------------------------
# Id allocation (stable across runs: kanto_ids.tsv / kanto_sprites.tsv)
# ---------------------------------------------------------------------------------------------
my (%sharedIdUsed, %flagsUsed, %varsUsed, %trainersUsed, %spritesUsed, %metatilesUsed, %localIdsDefined, %localIdsUsed, %specialsUsed, %stubsUsed, %macrosShimmed, %sharedUsed);
my %pendingNewIds; # name => kind, allocated at the end (sorted, so a run is deterministic)
sub kanto_flag { my $n = shift; $flagsUsed{$n} = 1; $pendingNewIds{$n} = 'flag' unless exists $idAlloc{$n}; return $n }
sub kanto_var  { my $n = shift; $varsUsed{$n} = 1; $pendingNewIds{$n} = 'var' unless exists $idAlloc{$n}; return $n }
sub allocate_ids
{
    my ($nextFlag, $nextVar) = (0, 0);
    for (keys %idAlloc) { if (/^FLAG_/) { $nextFlag = $idAlloc{$_} + 1 if $idAlloc{$_} >= $nextFlag } else { $nextVar = $idAlloc{$_} + 1 if $idAlloc{$_} >= $nextVar } }
    for my $n (sort keys %pendingNewIds)
    {
        next if exists $idAlloc{$n};
        $idAlloc{$n} = $pendingNewIds{$n} eq 'flag' ? $nextFlag++ : $nextVar++;
        push @idAllocOrder, $n;
    }
    %pendingNewIds = ();
    my $flagsAvail = $C{eventFlagsExtCount}; my $varsAvail = $C{varsExtCount};
    err("FLAG_KANTO_* overflow: $nextFlag flags allocated, EVENT_FLAGS_EXT holds $flagsAvail") if $nextFlag > $flagsAvail;
    err("VAR_KANTO_* overflow: $nextVar vars allocated, VARS_EXT holds $varsAvail") if $nextVar > $varsAvail;
}

# ---------------------------------------------------------------------------------------------
# Token translation
# ---------------------------------------------------------------------------------------------
my %ENGINE_VARS = map { $_ => 1 } qw(VAR_RESULT VAR_FACING VAR_LAST_TALKED);
my %KEEP_LOCALIDS = map { $_ => 1 } qw(LOCALID_PLAYER LOCALID_CAMERA LOCALID_NONE);
# pokefirered field-move objects keep Emerald's sprite and script (PS-096 kept rocks and trees that way; the card keeps boulders the same way)
my %FIELD_OBJECTS = (
    OBJ_EVENT_GFX_ROCK_SMASH_ROCK  => { gfx => 'OBJ_EVENT_GFX_BREAKABLE_ROCK',   script => 'EventScript_RockSmash',       what => 'rocks' },
    OBJ_EVENT_GFX_CUT_TREE         => { gfx => 'OBJ_EVENT_GFX_CUTTABLE_TREE',    script => 'EventScript_CutTree',         what => 'trees' },
    OBJ_EVENT_GFX_PUSHABLE_BOULDER => { gfx => 'OBJ_EVENT_GFX_PUSHABLE_BOULDER', script => 'EventScript_StrengthBoulder', what => 'boulders' },
);
my %fieldObjectScripts = map { $FIELD_OBJECTS{$_}{script} => 1 } keys %FIELD_OBJECTS;
# the macro shims in asm/macros/event.inc (hand-written, PS-100) and the imported labels they call
my %MACRO_DEPS = (giveitem_msg => ['Std_ReceivedItem'], msgreceiveditem => ['Std_ReceivedItem'], putitemaway => ['Std_PutItemAway'], set_gym_trainers => ['EventScript_SetGymTrainers']);
my %SHIM_MACROS = map { $_ => 1 } qw(famechecker goto_if_questlog set_gym_trainers giveitem_msg msgreceiveditem putitemaway trainerbattle_earlyrival checkmonmodernfatefulencounter setmonmodernfatefulencounter);
my %NOP_MACRO_ARGS = map { $_ => 1 } qw(famechecker goto_if_questlog); # their arguments are not emitted: FAMECHECKER_* etc. need no definition here

# a pokefirered label gets the Kanto prefix, unless shared_labels.tsv says Emerald's own script of the same meaning is used
sub kanto_label { my $l = shift; return $sharedLabels->{$l}{to} if exists $sharedLabels->{$l}; return $PFX . $l }

sub translate_music
{
    my $mus = shift;
    (my $rg = $mus) =~ s/^MUS_/MUS_RG_/;
    return $rg if $our{songs}{$rg};
    return $mus if $our{songs}{$mus};
    return $constTable->{$mus}{to} if exists $constTable->{$mus} && $our{songs}{$constTable->{$mus}{to}};
    return undef;
}

sub translate_flag
{
    my ($f, $where) = @_;
    return $f if $f =~ /^FLAG_TEMP_/;
    if (exists $sharedIds->{$f}) { $sharedIdUsed{$f} = $sharedIds->{$f}{to}; return $sharedIds->{$f}{to} =~ /^FLAG_${UPFX}_/ ? kanto_flag($sharedIds->{$f}{to}) : $sharedIds->{$f}{to} }
    if ($f =~ /^FLAG_WORLD_MAP_(\w+)$/)
    {
        my $visited = "FLAG_VISITED_${UPFX}_$1";
        return $visited if $C{visitedFlags}{$visited};
        return kanto_flag("FLAG_${UPFX}_WORLD_MAP_$1");
    }
    err("$where: flag $f is not defined in pokefirered") unless $fr{flags}{$f};
    (my $rest = $f) =~ s/^FLAG_//;
    return kanto_flag("FLAG_${UPFX}_$rest");
}
sub translate_var
{
    my ($v, $where) = @_;
    return $v if $v =~ /^VAR_TEMP_|^VAR_0x8|^VAR_OBJ_GFX_ID_/ || $ENGINE_VARS{$v};
    if (exists $sharedIds->{$v}) { $sharedIdUsed{$v} = $sharedIds->{$v}{to}; return $sharedIds->{$v}{to} =~ /^VAR_${UPFX}_/ ? kanto_var($sharedIds->{$v}{to}) : $sharedIds->{$v}{to} }
    err("$where: var $v is not defined in pokefirered") unless $fr{vars}{$v};
    (my $rest = $v) =~ s/^VAR_//;
    return kanto_var("VAR_${UPFX}_$rest");
}
sub translate_gfx
{
    my ($g, $where) = @_;
    return $g if $g =~ /^OBJ_EVENT_GFX_VAR/ || $g =~ /^\d+$/;
    return $FIELD_OBJECTS{$g}{gfx} if $FIELD_OBJECTS{$g};
    unless ($fr{gfx}{$g}) { err("$where: sprite $g is not defined in pokefirered"); return $g }
    unless ($frGfxInfo{$g}) { err("$where: sprite $g has no graphics info in pokefirered"); return $g }
    (my $rest = $g) =~ s/^OBJ_EVENT_GFX_//;
    my $ours = "OBJ_EVENT_GFX_${UPFX}_$rest";
    $spritesUsed{$ours} = $g;
    return $ours;
}
sub translate_special
{
    my ($s, $where) = @_;
    $specialsUsed{$s}++;
    my $row = $specialsTable->{$s};
    if ($row)
    {
        my $to = $row->{to};
        if ($to eq 'same') { err("specials.tsv: $s is marked 'same' but is not in data/specials.inc") unless $ourSpecials{$s}; return $s }
        if ($to =~ /^(stub|port):(\w+)$/) { $stubsUsed{$2}{$s}++; err("specials.tsv: $s -> $2 is not registered in data/specials.inc (src/kanto_specials.c)") unless $ourSpecials{$2}; return $2 }
        if ($to =~ /^=(\w+)$/) { err("specials.tsv: $s -> $1 is not in data/specials.inc") unless $ourSpecials{$1}; return $1 }
        err("specials.tsv: $s has an unknown action '$to' (same / =Name / stub:KantoName / port:KantoName)");
        return $s;
    }
    return $s if $ourSpecials{$s}; # the same name here: treated as the same meaning (reported)
    err("$where: special $s has no Emerald twin and no row in specials.tsv (" . ($frSpecials{$s} ? "pokefirered special" : "not even a pokefirered special") . ")");
    return $s;
}
sub translate_token
{
    my ($tok, $where, $opt) = @_;
    return $tok if $tok =~ /^\d/;                                   # numbers (0x0, 12)
    if (exists $label{$tok}) { $sharedUsed{$tok} = 1 if $label{$tok}{scope} eq 'shared'; return kanto_label($tok) }
    return $tok unless $tok =~ /^[A-Z][A-Z0-9_]*$/ || $tok =~ /^METATILE_/;  # macro names, lowercase words
    return $tok if $tok =~ /^(TRUE|FALSE|YES|NO|NULL)$/;
    if ($tok =~ /^FLAG_/) { return translate_flag($tok, $where) }
    if ($tok =~ /^VAR_/) { return translate_var($tok, $where) }
    if ($tok =~ /^TRAINER_/ && exists $fr{trainers}{$tok}) { $trainersUsed{$tok} = 1; (my $r = $tok) =~ s/^TRAINER_//; return "TRAINER_${UPFX}_$r" }
    if ($tok =~ /^LOCALID_/) { return $tok if $KEEP_LOCALIDS{$tok}; (my $r = $tok) =~ s/^LOCALID_//; my $l = "LOCALID_${UPFX}_$r"; $localIdsUsed{$l} = $where; return $l }
    if ($tok =~ /^OBJ_EVENT_GFX_/) { return translate_gfx($tok, $where) }
    if ($tok =~ /^MAP_/)
    {
        return $tok if $ourConstants{$tok} && !$C{frIdToName}{$tok};            # MAP_SCRIPT_*, MAP_DYNAMIC, MAP_GROUP ...
        if (my $name = $C{frIdToName}{$tok}) { return $C{our_map_id}->($tok) }
        err("$where: $tok is a pokefirered map outside the import"); return $tok;
    }
    if ($tok =~ /^HEAL_LOCATION_(\w+)$/) { my $h = "HEAL_LOCATION_${UPFX}_$1"; err("$where: $h is not an imported heal location") unless $C{healIds}{$h} || $tok eq 'HEAL_LOCATION_NONE'; return $tok eq 'HEAL_LOCATION_NONE' ? $tok : $h }
    if ($tok =~ /^LAYOUT_(\w+)$/) { my $l = "LAYOUT_${UPFX}_$1"; err("$where: $l is not an imported layout") unless $C{layoutIds}{$l}; return $l }
    if ($tok =~ /^METATILE_(\w+)$/) { err("$where: $tok is not in pokefirered's metatile_labels.h") unless exists $frMetatile{$tok}; $metatilesUsed{$tok} = 1; return "METATILE_${PFX}$1" }
    if ($tok =~ /^(MUS|SE)_/) { my $m = translate_music($tok); unless (defined $m) { err("$where: $tok has no track here (constants.tsv)"); return $tok } return $m }
    if ($tok =~ /^ITEM_((TM|HM)\d\d)_\w+$/) { return "ITEM_$1" }
    if ($tok =~ /^MOVETUTOR_(\w+)$/) { my $t = "TUTOR_MOVE_$1"; err("$where: $t is not a tutor move here") unless $our{tutors}{$t}; return $t }
    if (exists $scriptConsts->{$tok}) { return $scriptConsts->{$tok}{to} }
    if ($tok =~ /^(HELPCONTEXT|FAMECHECKER|FCPICKSTATE|QL_)/) { return '0' } # help system / fame checker: no-op shims ignore the value
    return $tok if $ourConstants{$tok} || $our{items}{$tok} || $our{species}{$tok} || $our{moves}{$tok} || $our{movement}{$tok} || $ourMacros{$tok};
    return $tok if $ourCharmapNames{$tok};                             # STR_VAR_1 etc.: preproc resolves charmap names in scripts
    return $tok if $opt && $opt->{equs}{$tok};                         # a .equ alias of the same file (copied with it)
    return $constTable->{$tok}{to} if exists $constTable->{$tok} && $ourConstants{$constTable->{$tok}{to}};
    err("$where: constant $tok is not defined here (script_constants.tsv / constants.tsv)");
    return $tok;
}
sub translate_code_tokens
{
    my ($code, $where, $opt) = @_;
    $code =~ s/\b([A-Za-z_]\w*)\b/translate_token($1, $where, $opt)/ge;
    return $code;
}

# .string "..." : charmap differences, placeholders, every character must exist here
sub translate_string
{
    my ($text, $where) = @_;
    for my $from (sort { length($b) <=> length($a) } keys %$textTable) { my $to = $textTable->{$from}{to}; $text =~ s/\Q$from\E/$to/g }
    $text =~ s/\{((?:MUS|SE)_[A-Z0-9_]+)\}/'{' . (translate_music_placeholder($1, $where)) . '}'/ge;
    my $check = $text;
    utf8::decode($check); # per-character check on code points (the files are UTF-8 bytes, written back as they are)
    $check =~ s/\\[nlp"]//g;
    while ($check =~ /\{([A-Z][A-Z0-9_]*)[^}]*\}/g) { err("$where: text placeholder {$1} is not in our charmap") unless $ourCharmapNames{$1} }
    $check =~ s/\{[^}]*\}//g;
    $check =~ s/\$$//;
    for my $ch (split //, $check) { next if $ourCharmapChars{$ch} || $ch eq '$'; my $bytes = $ch; utf8::encode($bytes); err("$where: character '$bytes' (U+" . sprintf("%04X", ord $ch) . ") is not in our charmap (text.tsv)"); $text =~ s/\Q$bytes\E/?/g }
    return $text;
}
sub translate_music_placeholder
{
    my ($m, $where) = @_;
    (my $rg = $m) =~ s/^MUS_/MUS_RG_/;
    return $rg if $ourCharmapNames{$rg};
    return $m if $ourCharmapNames{$m};
    err("$where: text placeholder {$m} has no song name in our charmap");
    return $m;
}

# one script line
sub translate_line
{
    my ($line, $where, $opt) = @_;
    return $line if $line !~ /\S/ || $line =~ /^\s*@/;
    if ($line =~ /^(\s*\.string\s+)"(.*)"(\s*(?:@.*)?)$/) { return $1 . '"' . translate_string($2, $where) . '"' . $3 }
    if ($line =~ /^(\w+)(::?)(\s*(?:@.*)?)$/) { return kanto_label($1) . $2 . $3 }
    my ($code, $comment) = $line =~ /^([^@]*?)(\s*@.*)?$/;
    $comment //= '';
    if ($code =~ /^(\s*\.equ\s+\w+\s*,\s*)(.*)$/) { return $1 . translate_code_tokens($2, $where, $opt) . $comment }
    if ($code =~ /^\s*\.(macro|endm|ifdef|ifndef|else|endif|align|if)\b/) { return $line }
    if ($code =~ /^\s*#\s*(ifdef|ifndef|if|else|endif)\b/) { return $line } # C preprocessor conditionals (#ifdef BUGFIX): event_scripts.s runs through cpp here too
    if ($code =~ /^(\s*)(\w+)(.*)$/)
    {
        my ($ind, $mac, $args) = ($1, $2, $3);
        # the macro must exist here: an event macro, a movement macro, a shim or a macro defined in the imported scripts
        unless ($ourMacros{$mac} || $ourMovementMacros{$mac} || $opt->{localMacros}{$mac} || $mac =~ /^\./)
        {
            err("$where: '$mac' is not a script macro here" . ($SHIM_MACROS{$mac} ? " (the PS-100 shim is missing from asm/macros/event.inc)" : ""));
            return $line;
        }
        $macrosShimmed{$mac}++ if $SHIM_MACROS{$mac};
        return $line if $NOP_MACRO_ARGS{$mac};
        if ($mac eq 'setworldmapflag' && $args =~ /^\s+(FLAG_\w+)\s*$/) { return "${ind}setflag " . translate_flag($1, $where) . $comment }
        if ($mac eq 'special' && $args =~ /^(\s+)(\w+)\s*$/) { return "${ind}special$1" . translate_special($2, $where) . $comment }
        if ($mac eq 'specialvar' && $args =~ /^(\s+)(\w+)(\s*,\s*)(\w+)\s*$/) { return "${ind}specialvar$1" . translate_code_tokens($2, $where, $opt) . $3 . translate_special($4, $where) . $comment }
        return $ind . $mac . translate_code_tokens($args, $where, $opt) . $comment;
    }
    return translate_code_tokens($code, $where, $opt) . $comment;
}

# ---------------------------------------------------------------------------------------------
# Closure: every label an event map needs
# ---------------------------------------------------------------------------------------------
my %needed;          # label => 1 (all event maps)
my %neededByMap;     # map => { label => 1 }
sub closure_from
{
    my ($roots, $m) = @_;
    my @queue = @$roots;
    while (my $l = shift @queue)
    {
        next if $neededByMap{$m}{$l};
        if (exists $sharedLabels->{$l}) { $sharedUsed{$l} = 1; next } # Emerald's own script is used (shared_labels.tsv)
        unless (exists $label{$l}) { err("map $m: script label $l is defined nowhere in pokefirered"); next }
        $neededByMap{$m}{$l} = 1; $needed{$l} = 1;
        my $file = $label{$l}{file};
        for my $line (@{$label{$l}{lines}})
        {
            next if $line =~ /^\s*\.string/ || $line =~ /^\s*@/;
            (my $code = $line) =~ s/@.*//;
            if ($code =~ /^\s*(\w+)/) { my $mac = $1; push @queue, "__macro__$mac" if exists $label{"__macro__$mac"}; push @queue, @{$MACRO_DEPS{$mac}} if $MACRO_DEPS{$mac}; next if $NOP_MACRO_ARGS{$mac} }
            for my $tok ($code =~ /\b([A-Za-z_]\w*)\b/g) { push @queue, $tok if exists $label{$tok} && !$neededByMap{$m}{$tok} }
        }
    }
}

# ---------------------------------------------------------------------------------------------
# Per map: events (map.json entries) and scripts.inc text
# ---------------------------------------------------------------------------------------------
my %perMap; # m => { objects, coord, bg, counts }
my %fieldObjectCounts;
sub map_events
{
    my ($m) = @_;
    my $fr = $C{maps}{$m}{fr};
    my $where = "map $m";
    my (@objects, @coords, @bgs);
    my %counts = (npc => 0, trainer => 0, item => 0, hidden => 0, sign => 0, trigger => 0, clone => 0, invisible => 0);
    my @roots = ("${m}_MapScripts", @{$extraRoots{$m} || []});
    err("$where: ${m}_MapScripts is not defined in pokefirered") unless exists $label{"${m}_MapScripts"};
    my $i = 0;
    for my $o (@{$fr->{object_events} || []})
    {
        $i++;
        my $w = "$where object $i" . (defined $o->{local_id} ? " ($o->{local_id})" : "") . " at ($o->{x},$o->{y})";
        if (($o->{type} // 'object') eq 'clone') { $counts{clone}++; note("$w: a clone of $o->{target_map}'s object dropped (no clone object events in this engine)"); next }
        my $g = $o->{graphics_id} // '0';
        my $script = $o->{script} // '0x0';
        my $gfx = translate_gfx($g, $w);
        if ($FIELD_OBJECTS{$g})
        {
            $fieldObjectCounts{$FIELD_OBJECTS{$g}{what}}++;
            note("$w: pokefirered script $script replaced by Emerald's $FIELD_OBJECTS{$g}{script}") if $script ne $FIELD_OBJECTS{$g}{script} && $script ne '0x0';
            $script = $FIELD_OBJECTS{$g}{script};
        }
        elsif ($script ne '0x0') { push @roots, $script; $script = kanto_label($script); $counts{($o->{trainer_type} // '') ne 'TRAINER_TYPE_NONE' ? 'trainer' : $g eq 'OBJ_EVENT_GFX_ITEM_BALL' ? 'item' : $g eq '0' ? 'invisible' : 'npc'}++ }
        else { $counts{$g eq '0' ? 'invisible' : 'npc'}++ }
        my $flag = translate_code_tokens($o->{flag} // '0', $w);
        my $mt = $o->{movement_type} // 'MOVEMENT_TYPE_NONE';
        unless ($our{movement}{$mt}) { if (exists $constTable->{$mt} && $our{movement}{$constTable->{$mt}{to}}) { note("$w: $mt -> $constTable->{$mt}{to} (constants.tsv)"); $mt = $constTable->{$mt}{to} } else { err("$w: movement type $mt does not exist here (constants.tsv)") } }
        my $tt = $o->{trainer_type} // 'TRAINER_TYPE_NONE';
        err("$w: trainer type $tt does not exist here") unless $ourConstants{$tt};
        my $sight = translate_code_tokens($o->{trainer_sight_or_berry_tree_id} // '0', $w);
        my @lines = ("    {");
        if (defined $o->{local_id}) { my $lid = translate_token($o->{local_id}, $w); err("$w: local id $lid is defined twice") if $localIdsDefined{$lid}; $localIdsDefined{$lid} = $m; push @lines, "      \"local_id\": \"$lid\"," }
        push @lines,
            "      \"graphics_id\": \"$gfx\",",
            "      \"x\": $o->{x},",
            "      \"y\": $o->{y},",
            "      \"elevation\": " . ($o->{elevation} // 0) . ",",
            "      \"movement_type\": \"$mt\",",
            "      \"movement_range_x\": " . ($o->{movement_range_x} // 0) . ",",
            "      \"movement_range_y\": " . ($o->{movement_range_y} // 0) . ",",
            "      \"trainer_type\": \"$tt\",",
            "      \"trainer_sight_or_berry_tree_id\": " . jstr($sight) . ",",
            "      \"script\": \"$script\",",
            "      \"flag\": \"$flag\"",
            "    }";
        push @objects, join("\n", @lines);
    }
    $i = 0;
    for my $c (@{$fr->{coord_events} || []})
    {
        $i++;
        my $w = "$where trigger $i at ($c->{x},$c->{y})";
        my $type = $c->{type} // 'trigger';
        if ($type ne 'trigger') { err("$w: coord event type $type is not handled"); next }
        my $script = $c->{script} // '0x0';
        if ($script ne '0x0') { push @roots, $script; $script = kanto_label($script) }
        my $var = translate_code_tokens($c->{var} // 'VAR_TEMP_0', $w);
        $counts{trigger}++;
        push @coords, join("\n", "    {", "      \"type\": \"trigger\",", "      \"x\": $c->{x},", "      \"y\": $c->{y},", "      \"elevation\": " . ($c->{elevation} // 0) . ",",
                           "      \"var\": \"$var\",", "      \"var_value\": " . jstr(translate_code_tokens("" . ($c->{var_value} // '0'), $w)) . ",", "      \"script\": \"$script\"", "    }");
    }
    $i = 0;
    for my $b (@{$fr->{bg_events} || []})
    {
        $i++;
        my $w = "$where bg event $i at ($b->{x},$b->{y})";
        if ($b->{type} eq 'sign')
        {
            my $script = $b->{script} // '0x0';
            if ($script ne '0x0') { push @roots, $script; $script = kanto_label($script) }
            err("$w: facing $b->{player_facing_dir} does not exist here") unless $ourConstants{$b->{player_facing_dir} // ''};
            $counts{sign}++;
            push @bgs, join("\n", "    {", "      \"type\": \"sign\",", "      \"x\": $b->{x},", "      \"y\": $b->{y},", "      \"elevation\": " . ($b->{elevation} // 0) . ",",
                            "      \"player_facing_dir\": \"$b->{player_facing_dir}\",", "      \"script\": \"$script\"", "    }");
        }
        elsif ($b->{type} eq 'hidden_item')
        {
            my $item = translate_token($b->{item} // 'ITEM_NONE', $w);
            err("$w: item $item does not exist here") unless $our{items}{$item};
            my $flag = translate_flag($b->{flag} // '', $w);
            note("$w: hidden $item x$b->{quantity}: Emerald's hidden items have no quantity, one is given") if ($b->{quantity} // 1) > 1;
            note("$w: hidden $item is 'underfoot' in pokefirered (Itemfinder only); here it is found like any hidden item") if $b->{underfoot};
            $counts{hidden}++;
            push @bgs, join("\n", "    {", "      \"type\": \"hidden_item\",", "      \"x\": $b->{x},", "      \"y\": $b->{y},", "      \"elevation\": " . ($b->{elevation} // 0) . ",",
                            "      \"item\": \"$item\",", "      \"flag\": \"$flag\"", "    }");
        }
        else { err("$w: bg event type $b->{type} is not handled") }
    }
    closure_from(\@roots, $m);
    $perMap{$m} = { objects => \@objects, coords => \@coords, bgs => \@bgs, counts => \%counts };
    return $perMap{$m};
}

# the text of data/maps/Kanto<m>/scripts.inc: the map's own labels in pokefirered's order (scripts.inc, then
# text.inc), translated; labels of other files go to kanto_shared.inc (or that map's file when it is imported too)
sub emit_labels
{
    my ($labels, $where) = @_;
    my (@out, %equs, %localMacros);
    my %files;
    for my $l (@$labels) { $files{$label{$l}{file}} = 1; $localMacros{$label{$l}{macro}} = 1 if $label{$l}{macro} }
    for my $f (sort { ($fileOrder{$a} // 9999) <=> ($fileOrder{$b} // 9999) || $a cmp $b } keys %files)
    {
        for my $e (@{$fileEqus{$f} || []}) { $equs{$e->[0]} = 1 }
    }
    my $opt = { equs => \%equs, localMacros => \%localMacros };
    my $lastFile = '';
    for my $l (sort { ($fileOrder{$label{$a}{file}} // 9999) <=> ($fileOrder{$label{$b}{file}} // 9999) || $label{$a}{file} cmp $label{$b}{file} || $label{$a}{line} <=> $label{$b}{line} } @$labels)
    {
        my $f = $label{$l}{file};
        if ($f ne $lastFile)
        {
            (my $rel = $f) =~ s/^\Q$frlg\E\///;
            push @out, "", "@ --- from pokefirered $rel ---";
            for my $e (@{$fileEqus{$f} || []}) { push @out, "\t.equ $e->[0], " . translate_code_tokens($e->[1], "$where ($rel .equ $e->[0])", $opt) }
            $lastFile = $f;
        }
        if ($label{$l}{macro})
        {
            push @out, "";
            push @out, map { translate_line($_, "$where (macro $label{$l}{macro})", $opt) } @{$label{$l}{lines}};
            next;
        }
        push @out, "", kanto_label($l) . "::";
        my @body = @{$label{$l}{lines}};
        pop @body while @body && $body[-1] !~ /\S/;
        push @out, map { translate_line($_, "$where ($l)", $opt) } @body;
    }
    return join("\n", @out) . "\n";
}

sub map_scripts
{
    my ($m) = @_;
    my @own = grep { $label{$_}{scope} eq "map:$m" } keys %{$neededByMap{$m}};
    my $text = "@ Generated by tools/kanto_import/kanto_import.pl --events (PS-100) from pokefirered data/maps/$m/scripts.inc + text.inc.\n"
             . "@ pokefirered's labels carry the Kanto prefix; flags, vars, trainers, local ids and specials are translated\n"
             . "@ (tools/kanto_import/README.md). Edit the converter or its tables, not this file.\n";
    $text .= emit_labels(\@own, "map $m");
    return $text;
}

# ---------------------------------------------------------------------------------------------
# After every map: shared scripts, ids, sprites, trainers, metatile labels
# ---------------------------------------------------------------------------------------------
sub finish
{
    my ($eventMaps) = @_;
    my %isEventMap = map { $_ => 1 } @$eventMaps;
    # shared labels: scope 'shared' or a map that is not an event map
    my @shared = grep { my $s = $label{$_}{scope}; $s eq 'shared' || ($s =~ /^map:(\w+)$/ && !$isEventMap{$1}) } keys %needed;
    my @otherMaps = grep { $label{$_}{scope} =~ /^map:(\w+)$/ && !$isEventMap{$1} } @shared;
    my $sharedText = "@ Generated by tools/kanto_import/kanto_import.pl --events (PS-100): the pokefirered shared scripts and text\n"
                   . "@ (data/scripts, data/text, data/event_scripts.s" . (@otherMaps ? ", and labels of maps outside the event import" : "") . ") that the imported Kanto maps\n"
                   . "@ reference, with the Kanto prefix. The macro shims giveitem_msg / msgreceiveditem / putitemaway / set_gym_trainers\n"
                   . "@ (asm/macros/event.inc) call KantoStd_ReceivedItem / KantoStd_PutItemAway / KantoEventScript_SetGymTrainers from here.\n"
                   . "\t.set FIRERED, 1 @ pokefirered's .ifdef FIRERED branches (FireRed data, like the wild encounters of PS-097)\n";
    $sharedText .= emit_labels(\@shared, "shared scripts");
    $C{queue_write}->("$repo/data/scripts/kanto_shared.inc", $sharedText);
    for my $mac (grep { $MACRO_DEPS{$_} } keys %macrosShimmed) { for my $dep (@{$MACRO_DEPS{$mac}}) { err("macro $mac needs $dep but it was not imported") unless $needed{$dep} } }
    # local ids referenced by scripts must be defined by an imported map's objects
    for my $l (sort keys %localIdsUsed) { err("$localIdsUsed{$l}: $l is not an object of any imported map (mapjson defines the LOCALID_ names from map.json)") unless $localIdsDefined{$l} }
    for my $m (sort keys %idAlloc) { }
    allocate_ids();
    # kanto_ids.tsv / kanto_sprites.tsv (stable allocations)
    {
        my $t = "# PS-100: FLAG_KANTO_* / VAR_KANTO_* offsets (hex) from EVENT_FLAGS_EXT_START / VARS_EXT_START, allocated by\n# tools/kanto_import/kanto_import.pl --events. Append-only: a name keeps its id forever (saves hold them). Never edit by hand.\n";
        $t .= sprintf("%s\t0x%X\n", $_, $idAlloc{$_}) for @idAllocOrder;
        $C{queue_write}->("$toolDir/kanto_ids.tsv", $t);
    }
    my @flagLines = map { sprintf("#define %-50s (EVENT_FLAGS_EXT_START + 0x%X)", $_, $idAlloc{$_}) } grep { /^FLAG_/ } @idAllocOrder;
    my @varLines  = map { sprintf("#define %-50s (VARS_EXT_START + 0x%X)", $_, $idAlloc{$_}) } grep { /^VAR_/ } @idAllocOrder;
    my $hdr = "// PS-100: generated by tools/kanto_import/kanto_import.pl --events from pokefirered's names (tools/kanto_import/kanto_ids.tsv keeps the ids stable)\n";
    $C{replace_marked_block}->("$repo/include/constants/flags.h", "// KANTO_EVENTS_BEGIN", "// KANTO_EVENTS_END", $hdr . join("\n", @flagLines) . (@flagLines ? "\n" : ""));
    $C{replace_marked_block}->("$repo/include/constants/vars.h", "// KANTO_EVENTS_BEGIN", "// KANTO_EVENTS_END", $hdr . join("\n", @varLines) . (@varLines ? "\n" : ""));
    # trainers
    finish_trainers($hdr);
    # metatile labels
    {
        my @lines = map { sprintf("#define %-48s %s", "METATILE_${PFX}" . substr($_, length('METATILE_')), $frMetatile{$_}) } sort keys %metatilesUsed;
        $C{replace_marked_block}->("$repo/include/constants/metatile_labels.h", "// KANTO_EVENTS_BEGIN", "// KANTO_EVENTS_END",
            "// PS-100: pokefirered's metatile labels used by the imported Kanto scripts (the prefix re-pack keeps pokefirered's ids), generated by tools/kanto_import --events\n" . join("\n", @lines) . (@lines ? "\n" : ""));
    }
    # sprites
    finish_sprites();
    # summary
    my @sum;
    my %tot;
    for my $m (@$eventMaps) { my $c = $perMap{$m}{counts}; $tot{$_} += $c->{$_} for keys %$c }
    push @sum, sprintf("events: %d maps: %d NPCs, %d trainers, %d item balls, %d invisible objects, %d clones dropped; %d signs, %d hidden items, %d triggers; field objects kept as Emerald's: %s",
                       scalar(@$eventMaps), $tot{npc}, $tot{trainer}, $tot{item}, $tot{invisible}, $tot{clone}, $tot{sign}, $tot{hidden}, $tot{trigger},
                       join(", ", map { "$fieldObjectCounts{$_} $_" } sort keys %fieldObjectCounts) || 'none');
    my ($ownLabels, $sharedLabels) = (0, scalar(@shared));
    for my $m (@$eventMaps) { $ownLabels += scalar(grep { $label{$_}{scope} eq "map:$m" } keys %{$neededByMap{$m}}) }
    push @sum, "events: scripts: $ownLabels map labels, $sharedLabels shared labels in data/scripts/kanto_shared.inc" . (@otherMaps ? " (" . scalar(@otherMaps) . " from maps outside the event import: " . join(", ", sort @otherMaps) . ")" : "");
    push @sum, "events: ids: " . scalar(grep { /^FLAG_/ } keys %flagsUsed) . " FLAG_KANTO_* and " . scalar(grep { /^VAR_/ } keys %varsUsed) . " VAR_KANTO_* (" . scalar(grep { /^FLAG_/ } @idAllocOrder) . " / " . scalar(grep { /^VAR_/ } @idAllocOrder) . " allocated in kanto_ids.tsv); through shared_ids.tsv: " . (join(", ", map { "$_->$sharedIdUsed{$_}" } sort keys %sharedIdUsed) || "none");
    push @sum, "events: specials: " . scalar(keys %specialsUsed) . " distinct called; same name here: " . join(", ", sort grep { !$specialsTable->{$_} && $ourSpecials{$_} } keys %specialsUsed) . "; through specials.tsv: " . join(", ", map { "$_->$specialsTable->{$_}{to}" } sort grep { $specialsTable->{$_} } keys %specialsUsed);
    push @sum, "events: stubs hit: " . (join(", ", map { my $s = $_; "$s <- " . join("/", sort keys %{$stubsUsed{$s}}) } sort keys %stubsUsed) || 'none');
    push @sum, "events: macro shims used: " . (join(", ", map { "$_ x$macrosShimmed{$_}" } sort keys %macrosShimmed) || 'none');
    push @sum, "events: trainers: " . scalar(keys %trainersUsed) . " TRAINER_KANTO_* (" . join(", ", sort keys %trainersUsed) . ")";
    push @sum, "events: sprites: " . scalar(keys %spritesUsed) . " OBJ_EVENT_GFX_KANTO_* (" . scalar(@spriteAllocOrder) . " allocated in kanto_sprites.tsv); metatile labels: " . scalar(keys %metatilesUsed);
    return @sum;
}

sub finish_trainers
{
    my ($hdr) = @_;
    my (@consts, @entries, @parties, %partyDone);
    my $base = $C{trainerKantoBase};
    my $rivalName = $trainersTable->{rival_name}{to} // 'GREEN';
    for my $t (sort { $fr{trainers}{$a} <=> $fr{trainers}{$b} } keys %trainersUsed)
    {
        my $id = $fr{trainers}{$t};
        (my $rest = $t) =~ s/^TRAINER_//;
        my $ours = "TRAINER_${UPFX}_$rest";
        my $value = $base + $id;
        for my $special (qw(TRAINER_FRONTIER_BRAIN TRAINER_PLAYER TRAINER_SECRET_BASE TRAINER_LINK_OPPONENT)) { err("$ours = $value collides with $special (raise the Kanto trainer base)") if $C{specialTrainerIds}{$special} && $C{specialTrainerIds}{$special} == $value }
        err("$ours = $value is beyond MAX_TRAINERS_TOTAL ($C{maxTrainersTotal}): TRAINER_FLAG() has no flag for it") if $value >= $C{maxTrainersTotal};
        push @consts, sprintf("#define %-44s (TRAINER_KANTO_BASE + %d) // pokefirered %s", $ours, $id, $t);
        my $text = $frTrainer{$t} or do { err("trainer $t has no gTrainers entry in pokefirered"); next };
        my $where = "trainer $t";
        $text =~ s/\[\Q$t\E\]/[$ours]/;
        if ($text =~ /\.trainerClass\s*=\s*(\w+)/) { my $c = $1; my $to = $our{classes}{$c} ? $c : $trainersTable->{$c}{to}; unless ($to && $our{classes}{$to}) { err("$where: class $c has no TRAINER_CLASS_ here (trainers.tsv)"); $to = $c } $text =~ s/\.trainerClass\s*=\s*\w+/.trainerClass = $to/ }
        if ($text =~ /\.trainerPic\s*=\s*(\w+)/) { my $p = $1; my $to = $our{pics}{$p} ? $p : $trainersTable->{$p}{to}; unless ($to && $our{pics}{$to}) { err("$where: pic $p has no TRAINER_PIC_ here (trainers.tsv)"); $to = $p } $text =~ s/\.trainerPic\s*=\s*\w+/.trainerPic = $to/ }
        if ($text =~ /\.encounterMusic_gender\s*=\s*([^,\n]+)/) { for my $tok ($1 =~ /\b([A-Z][A-Z0-9_]*)\b/g) { err("$where: $tok is not defined here") unless $ourConstants{$tok} } }
        if ($text =~ /\.trainerClass\s*=\s*(\w+)/ && ($1 eq 'TRAINER_CLASS_RIVAL' || $1 =~ /RIVAL/) && $frTrainer{$t} =~ /\.trainerClass\s*=\s*TRAINER_CLASS_(RIVAL_EARLY|RIVAL_LATE|CHAMPION)\b/ && $frTrainer{$t} =~ /\.trainerPic\s*=\s*TRAINER_PIC_(RIVAL_EARLY|RIVAL_LATE|CHAMPION_RIVAL)\b/)
        {
            $text =~ s/\.trainerName\s*=\s*_\("[^"]*"\)/.trainerName = _("$rivalName")/;
        }
        if ($text =~ /\.party\s*=\s*(\w+)\((sParty_\w+)\)/)
        {
            my ($kind, $party) = ($1, $2);
            my $p = $frParty{$party} or do { err("$where: party $party not found in pokefirered's trainer_parties.h"); next };
            (my $ourParty = $party) =~ s/^sParty_/sParty_${PFX}/;
            $text =~ s/\Q$party\E/$ourParty/;
            unless ($partyDone{$party}++)
            {
                my $pt = $p->{text};
                $pt =~ s/\Q$party\E/$ourParty/;
                for my $tok ($pt =~ /\b((?:SPECIES|MOVE|ITEM)_\w+)\b/g) { $tok = translate_token($tok, $where) if $tok =~ /^ITEM_(TM|HM)/; err("$where: $tok is not defined here") unless $our{species}{$tok} || $our{moves}{$tok} || $our{items}{$tok} }
                $pt =~ s/\b(ITEM_(?:TM|HM)\d\d)_\w+\b/$1/g;
                push @parties, $pt;
            }
        }
        for my $tok ($text =~ /\.items\s*=\s*\{([^}]*)\}/ ? ($1 =~ /\b(ITEM_\w+)\b/g) : ()) { err("$where: item $tok is not defined here") unless $our{items}{$tok} }
        $text =~ s/,\s*$//;
        push @entries, "    " . $text . ",";
    }
    $C{replace_marked_block}->("$repo/include/constants/opponents.h", "// KANTO_EVENTS_BEGIN", "// KANTO_EVENTS_END",
        $hdr . "// TRAINER_KANTO_* = TRAINER_KANTO_BASE + pokefirered's trainer id (stable by construction); flags through TRAINER_FLAG() (PS-070)\n" . join("\n", @consts) . (@consts ? "\n" : ""));
    my $gen = "// PS-100: generated by tools/kanto_import/kanto_import.pl --events from pokefirered's src/data/trainers.h (classes / pics through\n// tools/kanto_import/trainers.tsv when Emerald has no twin). Included inside gTrainers[] (src/data/trainers.h). Do not edit.\n";
    $C{queue_write}->("$repo/src/data/kanto_trainers.h", $gen . join("\n", @entries) . (@entries ? "\n" : ""));
    $C{queue_write}->("$repo/src/data/kanto_trainer_parties.h", "// PS-100: generated by tools/kanto_import/kanto_import.pl --events from pokefirered's src/data/trainer_parties.h. Do not edit.\n" . join("\n\n", @parties) . (@parties ? "\n" : ""));
}

# pokefirered sprites -> OBJ_EVENT_GFX_KANTO_*: png copies, pic tables, graphics infos, pointer table entries
my %PAL_TAG = (
    OBJ_EVENT_PAL_TAG_NPC_BLUE => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_BLUE', OBJ_EVENT_PAL_TAG_NPC_PINK => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_PINK',
    OBJ_EVENT_PAL_TAG_NPC_GREEN => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_GREEN', OBJ_EVENT_PAL_TAG_NPC_WHITE => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_WHITE',
    OBJ_EVENT_PAL_TAG_NPC_BLUE_REFLECTION => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_BLUE_REFLECTION', OBJ_EVENT_PAL_TAG_NPC_PINK_REFLECTION => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_PINK_REFLECTION',
    OBJ_EVENT_PAL_TAG_NPC_GREEN_REFLECTION => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_GREEN_REFLECTION', OBJ_EVENT_PAL_TAG_NPC_WHITE_REFLECTION => 'OBJ_EVENT_PAL_TAG_KANTO_NPC_WHITE_REFLECTION',
    OBJ_EVENT_PAL_TAG_SS_ANNE => 'OBJ_EVENT_PAL_TAG_SS_ANNE', OBJ_EVENT_PAL_TAG_SEAGALLOP => 'OBJ_EVENT_PAL_TAG_KANTO_SEAGALLOP', OBJ_EVENT_PAL_TAG_METEORITE => 'OBJ_EVENT_PAL_TAG_KANTO_METEORITE',
    OBJ_EVENT_PAL_TAG_NONE => 'OBJ_EVENT_PAL_TAG_NONE',
);
# the palettes those tags name (copied every run; the table entries are hand-written in event_object_movement.c)
my @KANTO_PALETTES = qw(npc_blue npc_pink npc_green npc_white npc_blue_reflection npc_pink_reflection npc_green_reflection npc_white_reflection seagallop meteorite);
my %ANIMS = (sAnimTable_Standard => 1, sAnimTable_Inanimate => 1, sAnimTable_Nurse => 1, sAnimTable_HoOh => 1, sAnimTable_RockSmashRock => 'sAnimTable_BreakableRock', sAnimTable_CutTree => 'sAnimTable_CuttableTree');
sub finish_sprites
{
    my (@gfxLines, @picDecls, @picTables, @infos, @pointers);
    # allocation
    my $next = 0;
    for (values %spriteAlloc) { $next = $_ + 1 if $_ + 1 > $next }
    for my $s (sort keys %spritesUsed) { next if exists $spriteAlloc{$s}; $spriteAlloc{$s} = $next++; push @spriteAllocOrder, $s }
    {
        my $t = "# PS-100: OBJ_EVENT_GFX_KANTO_* indices (OBJ_EVENT_GFX_KANTO_FIRST + n), allocated by tools/kanto_import/kanto_import.pl --events.\n# Append-only: a sprite keeps its id forever (saves hold graphics ids). Never edit by hand.\n";
        $t .= "$_\t$spriteAlloc{$_}\n" for @spriteAllocOrder;
        $C{queue_write}->("$toolDir/kanto_sprites.tsv", $t);
    }
    my $count = $next; # one past the highest allocated index
    push @gfxLines, map { sprintf("#define %-44s (OBJ_EVENT_GFX_KANTO_FIRST + %d)%s", $_, $spriteAlloc{$_}, $spritesUsed{$_} ? "" : " // allocated by an earlier run, not used by the current map list") } @spriteAllocOrder;
    push @gfxLines, sprintf("#define %-44s %d", "OBJ_EVENT_GFX_KANTO_COUNT", $count);
    $C{replace_marked_block}->("$repo/include/constants/event_objects.h", "// KANTO_EVENTS_BEGIN", "// KANTO_EVENTS_END", join("\n", @gfxLines) . "\n");
    # palettes (always, the hand-written table in event_object_movement.c references them)
    for my $p (@KANTO_PALETTES)
    {
        my $src = "$frlg/graphics/object_events/palettes/$p.pal";
        unless (-f $src) { err("pokefirered palette $p.pal is missing"); next }
        my $text = slurp($src); $text =~ s/\n/\r\n/g; # JASC-PAL files are CRLF in both repositories
        $C{queue_write}->("$repo/graphics/object_events/palettes/kanto_$p.pal", $text);
        my $sym = 'gObjectEventPal_' . $PFX . join('', map { ucfirst } split /_/, $p);
        push @picDecls, "const u16 ${sym}[] = INCGFX_U16(\"graphics/object_events/palettes/kanto_$p.pal\", \".gbapal\");";
    }
    my %infoDone;
    for my $ours (sort { $spriteAlloc{$a} <=> $spriteAlloc{$b} } grep { $spritesUsed{$_} } keys %spriteAlloc)
    {
        my $frg = $spritesUsed{$ours};
        my $infoSym = $frGfxInfo{$frg};
        my $info = $frInfo{$infoSym} or do { err("sprite $frg: $infoSym is not in pokefirered's object_event_graphics_info.h"); next };
        (my $ourInfo = $infoSym) =~ s/^gObjectEventGraphicsInfo_/gObjectEventGraphicsInfo_$PFX/;
        push @pointers, sprintf("    [%-44s = &%s,", "$ours]", $ourInfo);
        next if $infoDone{$infoSym}++;
        my $where = "sprite $frg";
        my $picTable = $info->{images} // '';
        my $frames = $frPicTable{$picTable} or do { err("$where: pic table $picTable not found"); next };
        (my $ourPicTable = $picTable) =~ s/^sPicTable_/sPicTable_$PFX/;
        my (%pics, @entries);
        for my $e (@$frames)
        {
            if ($e =~ /^overworld_frame\((gObjectEventPic_\w+),\s*(\d+),\s*(\d+),\s*(\d+)\),?$/)
            {
                my ($pic, $w, $h, $n) = ($1, $2, $3, $4);
                $pics{$pic} = [$w, $h];
                (my $p = $pic) =~ s/^gObjectEventPic_/gObjectEventPic_$PFX/;
                push @entries, "    overworld_frame($p, $w, $h, $n),";
            }
            elsif ($e =~ /^obj_frame_tiles\((gObjectEventPic_\w+)\),?$/)
            {
                my $pic = $1;
                $pics{$pic} //= undef;
                (my $p = $pic) =~ s/^gObjectEventPic_/gObjectEventPic_$PFX/;
                push @entries, "    obj_frame_tiles($p),";
            }
            else { err("$where: pic table entry '$e' not understood") }
        }
        for my $pic (sort keys %pics)
        {
            my $path = $frPicPath{$pic} or do { err("$where: $pic has no INCBIN in pokefirered's object_event_graphics.h"); next };
            my $src = "$frlg/$path.png";
            unless (-f $src) { err("$where: $src is missing"); next }
            (my $rel = $path) =~ s{^graphics/object_events/pics/}{};
            my $dst = "graphics/object_events/pics/kanto/$rel.png";
            $C{queue_write}->("$repo/$dst", slurp_raw($src));
            my $opts = $frPngOpts{"$path"};
            if (!defined $opts && $pics{$pic} && @$frames > 1) { $opts = "-mwidth $pics{$pic}[0] -mheight $pics{$pic}[1]" }
            (my $ourPic = $pic) =~ s/^gObjectEventPic_/gObjectEventPic_$PFX/;
            push @picDecls, "const u32 ${ourPic}[] = INCGFX_U32(\"$dst\", \".4bpp\"" . (defined $opts ? ", \"$opts\"" : "") . ");";
        }
        push @picTables, "static const struct SpriteFrameImage ${ourPicTable}[] = {\n" . join("\n", @entries) . "\n};";
        my $palTag = $PAL_TAG{$info->{paletteTag} // ''} or err("$where: palette tag $info->{paletteTag} has no Kanto palette here (player palettes are not imported)");
        my $reflTag = $PAL_TAG{$info->{reflectionPaletteTag} // ''} or err("$where: reflection palette tag $info->{reflectionPaletteTag} unknown");
        my $anims = $info->{anims} // ''; my $ourAnims = $ANIMS{$anims}; $ourAnims = $anims if defined $ourAnims && $ourAnims eq '1';
        err("$where: animation table $anims has no twin here") unless $ourAnims;
        my $oam = $info->{oam} // ''; err("$where: $oam is not a base OAM here") unless $oam =~ /^&gObjectEventBaseOam_(8x8|16x16|16x32|32x16|32x32|64x64)$/; # 32x16: ported for pokefirered's Town Map (PS-100)
        (my $sub = $info->{subspriteTables} // '') =~ s/^gObjectEventSpriteOamTables_/sOamTables_/; err("$where: subsprite table $info->{subspriteTables} has no twin here") unless $sub =~ /^sOamTables_(16x16|16x32|32x32|64x64|128x64)$/;
        for my $k (qw(tileTag size width height paletteSlot shadowSize inanimate disableReflectionPaletteLoad tracks)) { err("$where: field $k missing") unless defined $info->{$k} }
        push @infos, join("\n",
            "const struct ObjectEventGraphicsInfo $ourInfo = { // pokefirered $infoSym",
            "    .tileTag = $info->{tileTag},",
            "    .paletteTag = " . ($palTag // 'OBJ_EVENT_PAL_TAG_NONE') . ",",
            "    .reflectionPaletteTag = " . ($reflTag // 'OBJ_EVENT_PAL_TAG_NONE') . ",",
            "    .size = $info->{size},",
            "    .width = $info->{width},",
            "    .height = $info->{height},",
            "    .paletteSlot = $info->{paletteSlot},",
            "    .shadowSize = $info->{shadowSize},",
            "    .inanimate = $info->{inanimate},",
            "    .disableReflectionPaletteLoad = $info->{disableReflectionPaletteLoad},",
            "    .tracks = $info->{tracks},",
            "    .oam = $oam,",
            "    .subspriteTables = $sub,",
            "    .anims = " . ($ourAnims // $anims) . ",",
            "    .images = $ourPicTable,",
            "    .affineAnims = gDummySpriteAffineAnimTable,",
            "};");
    }
    my $gen = "// PS-100: generated by tools/kanto_import/kanto_import.pl --events from pokefirered's src/data/object_events (sprites of the imported\n// Kanto maps, pictures copied to graphics/object_events/pics/kanto, pokefirered's NPC palettes as kanto_npc_*.pal). Do not edit.\n";
    $C{queue_write}->("$repo/src/data/object_events/kanto/graphics.h", $gen . join("\n", @picDecls) . "\n");
    $C{queue_write}->("$repo/src/data/object_events/kanto/pic_tables.h", $gen . join("\n\n", @picTables) . "\n");
    $C{queue_write}->("$repo/src/data/object_events/kanto/graphics_info.h", $gen . join("\n\n", @infos) . "\n");
    $C{queue_write}->("$repo/src/data/object_events/kanto/graphics_info_pointers.h", $gen . "// Entries of gObjectEventGraphicsInfoPointers[] (src/data/object_events/object_event_graphics_info_pointers.h includes this inside the array)\n" . join("\n", @pointers) . "\n");
}

1;
