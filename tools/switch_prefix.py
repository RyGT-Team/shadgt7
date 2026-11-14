#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""FEAT-GT7-175: the switch prefixes, and a tool to apply them.

Environment switches are named after where they take effect:

  SHADGT_*  read only by the renderer (shadGT / shadgt7);
  RYGT_*    read only by the runtime (RyGTS / ryGT7: core/, app/, plugins/);
  SHARED_*  read by both.

Usage:
  switch_prefix.py --game gts|gt7 [--check] [--write-map FILE] RENDERER_TREE
      Renames, in the renderer tree, every RYGT_ switch the renderer reads (a "RYGT_..." string
      literal in the C/C++ sources, or a renderer row of src/video_core/rygt_switches.cpp) to
      SHADGT_, and the shared ones to SHARED_. Mentions in comments, docs and scripts are renamed
      too; CHANGES.MD / CHANGELOG.md keep the names of their time. Running it again is a no-op,
      so a branch that adds RYGT_ switches can run it after merging.
      --check only reports what would change (exit 1 if anything would).
      --write-map writes {"map": {old: new}, "names": [every switch]} as JSON, for the runtime and
      launcher repositories.
  switch_prefix.py --apply-map FILE [--check] TREE
      Applies a map written by --write-map to another tree (the RyGTS / ryGT7 repository: the
      launcher sets and lists renderer switches, the runtime reads the shared ones).

A name that is not a switch (RYGT_SWITCH_EXPORT, RYGT_DLSS_QUALITY_*, CMake options, ...) is never
touched: only the exact names in the map are, plus wildcard mentions (RYGT_PARALLEL_*) whose
switches all move to the same prefix.
"""
import argparse, json, os, re, sys

# Read by the runtime's own code (core/, app/, plugins/). These stay RYGT_ ...
RUNTIME = {
    'gts': ['RYGT_APP0_LAYERS', 'RYGT_AVP_TRACE', 'RYGT_BATCH_MAP_MERGE', 'RYGT_HDD_STREAMING', 'RYGT_LOG_DIRECTORY',
            'RYGT_MOVIES', 'RYGT_MUTEX_SPIN', 'RYGT_USER_NAME', 'RYGT_AMD_SOFTWARE'],
    'gt7': ['RYGT_APP0_LAYERS', 'RYGT_AVP_TRACE', 'RYGT_BATCHMAP_MERGE', 'RYGT_COND_TARGETED', 'RYGT_DEFER_UNMAP',
            'RYGT_DIAG_BATCHMAP', 'RYGT_DIAG_DEPENDENCIES', 'RYGT_DIAG_MAPPING_FILE', 'RYGT_FAST_NATIVE_MAPPINGS',
            'RYGT_FIBER_HOST_STACKS', 'RYGT_HDR_DISPLAY_NITS', 'RYGT_JPEG_SUBSAMPLED_CAP', 'RYGT_LOG_DIRECTORY',
            'RYGT_MAPPING_HANDLE_REUSE', 'RYGT_MAPPING_QUERY_REUSE', 'RYGT_MAPPING_VIEW_BOUNDS', 'RYGT_MAPPING_VIEW_CHUNK',
            'RYGT_MOVIES', 'RYGT_ONLINE', 'RYGT_ONLINE_ADDRESS', 'RYGT_ONLINE_ID', 'RYGT_PCORES', 'RYGT_PHOTO_EXPORT',
            'RYGT_PLACEHOLDER_CACHE', 'RYGT_STAGED_GUEST_READS', 'RYGT_USER_NAME'],
}
# ... and these are read by the runtime and the renderer: SHARED_.
SHARED = {
    'gts': ['RYGT_DIAG'],
    'gt7': ['RYGT_AMD_SINGLE_SAMPLE_BARYCENTRIC', 'RYGT_ASPECT', 'RYGT_DIAG', 'RYGT_DLSSG_FRAMES', 'RYGT_DRIVER_CACHE_MAX_MB',
            'RYGT_GAME_LOG', 'RYGT_NATIVE_INPLACE_FROM', 'RYGT_NATIVE_INPLACE_RANGES', 'RYGT_NEO', 'RYGT_ONLINE_SERVER',
            'RYGT_P2P_PORT', 'RYGT_TEXTURE_LOD_BIAS', 'RYGT_UPSCALE_QUALITY', 'SHADGT_FPS_LOCK'],
}

SOURCE_EXT = {'.cpp', '.h', '.hpp', '.c', '.cc', '.inl'}
TEXT_EXT = SOURCE_EXT | {'.py', '.cmake', '.txt', '.md', '.ps1', '.cmd', '.bat', '.sh', '.yml', '.yaml', '.json', '.cs',
                         '.ini', '.toml', '.glsl', '.comp', '.frag', '.vert', '.in', '.html', '.js'}
SKIP_DIRS = {'.git', 'externals', '3rdparty', 'Build', 'build', 'out', 'node_modules', '.vs'}
KEEP_FILES = {'changes.md', 'changelog.md'}  # history keeps the names of its time
LITERAL = re.compile(r'L?"((?:RYGT|SHADGT)_[A-Z0-9_]*[A-Z0-9])"')
TABLE = re.compile(r'"RYGTSW1\|([A-Z0-9_]+)\|[^|]*\|([^|]*)\|')
TOKEN = re.compile(r'(?<![A-Za-z0-9_])((?:RYGT|SHADGT)_[A-Z0-9_]*)')


def files(tree):
    for base, dirs, names in os.walk(tree):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in names:
            ext = os.path.splitext(name)[1].lower()
            if ext in TEXT_EXT or name == 'CMakeLists.txt':
                yield os.path.join(base, name)


def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return None


def suffix(name):
    return name.split('_', 1)[1]


def renderer_map(tree, game):
    runtime, shared = set(RUNTIME[game]), set(SHARED[game])
    names = set()
    for path in files(tree):
        if os.path.splitext(path)[1].lower() not in SOURCE_EXT:
            continue
        text = read(path)
        if text is None:
            continue
        names.update(LITERAL.findall(text))
        for name, group in TABLE.findall(text):
            if group != 'Runtime':
                names.add(name)
    mapping = {}
    for name in sorted(names):
        if name in shared:
            new = 'SHARED_' + suffix(name)
        elif name.startswith('RYGT_') and name not in runtime:
            new = 'SHADGT_' + suffix(name)
        else:
            continue
        mapping[name] = new
    # Two switches must not end up with one name.
    final = {}
    for name in names:
        final.setdefault(mapping.get(name, name), []).append(name)
    clashes = {k: v for k, v in final.items() if len(v) > 1}
    if clashes:
        sys.exit('switch_prefix: these would share a name: %s' % clashes)
    return mapping, sorted(names)


def rename_text(text, mapping, names):
    def replace(match):
        token = match.group(1)
        if token in mapping:
            return mapping[token]
        # A wildcard mention (RYGT_PARALLEL_*, "RYGT_DIAG_..."): renamed when it has two words or more
        # and every switch under it moves to the same prefix. A bare prefix (RYGT_, SHADGT_) never is.
        if token.endswith('_') and token.count('_') >= 2:
            under = [n for n in names if n.startswith(token)]
            targets = set(mapping[n].split('_', 1)[0] if n in mapping else '' for n in under)
            if under and len(targets) == 1 and '' not in targets:
                return targets.pop() + '_' + suffix(token)
        return token
    return TOKEN.sub(replace, text)


def apply(tree, mapping, names, check):
    changed = []
    for path in files(tree):
        if os.path.basename(path).lower() in KEEP_FILES:
            continue
        text = read(path)
        if text is None:
            continue
        new = rename_text(text, mapping, names)
        if new != text:
            changed.append(os.path.relpath(path, tree))
            if not check:
                with open(path, 'wb') as f:
                    f.write(new.encode('utf-8'))
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', choices=sorted(RUNTIME))
    parser.add_argument('--apply-map')
    parser.add_argument('--write-map')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('tree')
    args = parser.parse_args()
    if args.apply_map:
        data = json.load(open(args.apply_map, encoding='utf-8'))
        mapping, names = data['map'], data['names']
    elif args.game:
        mapping, names = renderer_map(args.tree, args.game)
    else:
        parser.error('--game or --apply-map is needed')
    if args.write_map:
        with open(args.write_map, 'w', encoding='utf-8') as f:
            json.dump({'map': mapping, 'names': names}, f, indent=1, sort_keys=True)
    changed = apply(args.tree, mapping, names, args.check)
    print('%d switches renamed; %d files %s' % (len(mapping), len(changed), 'would change' if args.check else 'changed'))
    for path in changed:
        print('  ' + path)
    return 1 if args.check and changed else 0


if __name__ == '__main__':
    sys.exit(main())
