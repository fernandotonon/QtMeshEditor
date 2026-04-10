import React, { useState, useEffect } from 'react';
import s from './DocsApp.module.css';

const NAV = [
  { section: 'Getting Started', items: [
    { id: 'installation', label: 'Installation' },
    { id: 'quick-start', label: 'Quick Start' },
  ]},
  { section: 'CLI Commands', items: [
    { id: 'cmd-info', label: 'info' },
    { id: 'cmd-fix', label: 'fix' },
    { id: 'cmd-convert', label: 'convert' },
    { id: 'cmd-anim', label: 'anim' },
    { id: 'cmd-validate', label: 'validate' },
    { id: 'cmd-lod', label: 'lod' },
    { id: 'cmd-pose', label: 'pose' },
    { id: 'cmd-scan', label: 'scan' },
  ]},
  { section: 'Scan Reference', items: [
    { id: 'scan-config', label: 'Configuration (qtmesh.yml)' },
    { id: 'scan-rules', label: 'Rules Reference' },
    { id: 'scan-scopes', label: 'Scoped Validation' },
    { id: 'scan-output', label: 'Output Formats' },
    { id: 'scan-fix', label: 'Auto-Fix' },
  ]},
  { section: 'Integration', items: [
    { id: 'docker', label: 'Docker' },
    { id: 'github-actions', label: 'GitHub Actions' },
    { id: 'gitlab-ci', label: 'GitLab CI' },
    { id: 'ci-cd', label: 'CI/CD Patterns' },
  ]},
];

function Code({ children }) {
  return <code className={s.code}>{children}</code>;
}

function CodeBlock({ children, lang }) {
  return (
    <pre className={`${s.codeBlock} ${lang === 'yaml' ? s.yamlExample : ''}`}>
      <code>{children}</code>
    </pre>
  );
}

function Badge({ type, children }) {
  const cls = type === 'error' ? s.badgeError : type === 'warning' ? s.badgeWarning : s.badgeInfo;
  return <span className={`${s.badge} ${cls}`}>{children}</span>;
}

function RuleCard({ name, type, severity, description, example, children }) {
  return (
    <div className={s.ruleCard} id={`rule-${name}`}>
      <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem', marginBottom: '0.4rem' }}>
        <strong style={{ fontFamily: 'var(--mono)', fontSize: '0.95rem' }}>{name}</strong>
        <Badge type={severity}>{severity}</Badge>
        <span className={s.code} style={{ fontSize: '0.75rem' }}>{type}</span>
      </div>
      <p className={s.para} style={{ margin: '0.3rem 0' }}>{description}</p>
      {example && <CodeBlock lang="yaml">{example}</CodeBlock>}
      {children}
    </div>
  );
}

function CmdSection({ id, name, synopsis, description, examples, options, children }) {
  return (
    <section className={s.section} id={id}>
      <h2 className={s.sectionTitle}>{name}</h2>
      <p className={s.para}>{description}</p>
      {synopsis && <CodeBlock>{synopsis}</CodeBlock>}
      {options && options.length > 0 && (
        <>
          <h3 className={s.subsection}>Options</h3>
          <table className={s.table}>
            <thead><tr><th>Flag</th><th>Description</th></tr></thead>
            <tbody>
              {options.map(([flag, desc], i) => <tr key={i}><td><Code>{flag}</Code></td><td>{desc}</td></tr>)}
            </tbody>
          </table>
        </>
      )}
      {examples && examples.length > 0 && (
        <>
          <h3 className={s.subsection}>Examples</h3>
          {examples.map((ex, i) => <CodeBlock key={i}>{ex}</CodeBlock>)}
        </>
      )}
      {children}
    </section>
  );
}

export default function DocsApp() {
  const [active, setActive] = useState('installation');
  const [menuOpen, setMenuOpen] = useState(false);

  useEffect(() => {
    const observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) { setActive(entry.target.id); break; }
        }
      },
      { rootMargin: '-80px 0px -60% 0px', threshold: 0 }
    );
    document.querySelectorAll('section[id]').forEach(el => observer.observe(el));
    return () => observer.disconnect();
  }, []);

  return (
    <div className={s.layout}>
      <button className={s.mobileToggle} onClick={() => setMenuOpen(!menuOpen)} aria-label="Toggle menu">
        <span /><span /><span />
      </button>

      {menuOpen && <div className={s.mobileOverlay} onClick={() => setMenuOpen(false)} />}

      <aside className={`${s.sidebar} ${menuOpen ? s.sidebarOpen : ''}`}>
        <div className={s.sidebarHeader}>
          <a href="./index.html" style={{ color: 'var(--accent-1)', textDecoration: 'none', fontWeight: 700, fontSize: '1.05rem' }}>
            qtmesh
          </a>
          <span style={{ color: 'var(--text-muted)', fontSize: '0.85rem', marginLeft: '0.5rem' }}>docs</span>
        </div>
        <nav className={s.sidebarNav}>
          {NAV.map(group => (
            <div key={group.section}>
              <div className={s.navSection}>{group.section}</div>
              {group.items.map(item => (
                <a
                  key={item.id}
                  href={`#${item.id}`}
                  className={`${s.navLink} ${active === item.id ? s.active : ''}`}
                  onClick={() => setMenuOpen(false)}
                >
                  {item.label}
                </a>
              ))}
            </div>
          ))}
        </nav>
      </aside>

      <main className={s.main}>
        <div className={s.topBar}>
          <div className={s.topBarBrand}>
            <p className={s.topBarKicker}>Documentation</p>
            <h1 className={s.topBarTitle}>qtmesh CLI Reference</h1>
          </div>
          <nav className={s.topBarActions} aria-label="Documentation links">
            <a href="./index.html" className={s.topBarLinkSecondary}>
              Back to Main Page
            </a>
            <a href="https://github.com/fernandotonon/QtMeshEditor" target="_blank" rel="noopener" className={s.topBarLink}>
              GitHub
            </a>
          </nav>
        </div>

        <div className={s.mainInner}>

          {/* ─── Getting Started ─── */}

          <section className={s.section} id="installation">
            <h2 className={s.sectionTitle}>Installation</h2>
            <p className={s.para}>qtmesh is the command-line interface bundled with QtMeshEditor. It supports 40+ 3D formats and runs on Windows, macOS, and Linux.</p>

            <h3 className={s.subsection}>Package Managers</h3>
            <CodeBlock>{`# Windows (WinGet)
winget install FernandoTonon.QtMeshEditor --source winget

# macOS (Homebrew)
brew tap fernandotonon/qtmesheditor
brew install qtmesheditor

# Linux (Snap)
sudo snap install qtmesheditor

# Docker
docker pull ghcr.io/fernandotonon/qtmesh`}</CodeBlock>

            <h3 className={s.subsection}>From Source</h3>
            <CodeBlock>{`git clone https://github.com/fernandotonon/QtMeshEditor.git
cd QtMeshEditor
cmake . -B build -DCMAKE_PREFIX_PATH="/path/to/Qt;/path/to/ogre/SDK"
cmake --build build --target QtMeshEditor -j4
# The 'qtmesh' symlink is created automatically`}</CodeBlock>
          </section>

          <section className={s.section} id="quick-start">
            <h2 className={s.sectionTitle}>Quick Start</h2>
            <CodeBlock>{`# Inspect a model
qtmesh info character.fbx --json

# Convert formats
qtmesh convert character.dae -o character.gltf2

# Merge animations from multiple files
qtmesh anim base.fbx --merge walk.fbx run.fbx -o merged.fbx

# Validate mesh geometry
qtmesh validate character.fbx

# Scan a directory for asset issues
qtmesh scan ./assets --fail-on error`}</CodeBlock>

            <h3 className={s.subsection}>Exit Codes</h3>
            <table className={s.table}>
              <thead><tr><th>Code</th><th>Meaning</th></tr></thead>
              <tbody>
                <tr><td><Code>0</Code></td><td>Success (or all assets pass scan)</td></tr>
                <tr><td><Code>1</Code></td><td>Runtime error or findings exceed threshold</td></tr>
                <tr><td><Code>2</Code></td><td>Usage error (bad arguments)</td></tr>
              </tbody>
            </table>

            <h3 className={s.subsection}>Global Options</h3>
            <table className={s.table}>
              <thead><tr><th>Flag</th><th>Description</th></tr></thead>
              <tbody>
                <tr><td><Code>--help, -h</Code></td><td>Show help</td></tr>
                <tr><td><Code>--version, -v</Code></td><td>Show version</td></tr>
                <tr><td><Code>--verbose</Code></td><td>Show engine debug output</td></tr>
                <tr><td><Code>--no-telemetry</Code></td><td>Permanently disable anonymous usage data</td></tr>
              </tbody>
            </table>
          </section>

          {/* ─── CLI Commands ─── */}

          <CmdSection id="cmd-info" name="info" description="Show detailed mesh information: vertex/face counts, materials, textures, skeleton, animations, bounding box."
            synopsis="qtmesh info <file> [--json]"
            options={[['--json', 'Output as JSON instead of text']]}
            examples={['qtmesh info character.fbx', 'qtmesh info character.fbx --json']}
          />

          <CmdSection id="cmd-fix" name="fix" description="Re-import and export a mesh through Assimp with standard optimizations (join identical vertices, smooth normals). Optionally remove degenerate triangles or merge redundant materials."
            synopsis="qtmesh fix <file> [-o <output>] [flags]"
            options={[
              ['-o <output>', 'Output file path (overwrites input if omitted)'],
              ['--remove-degenerates', 'Remove degenerate (zero-area) triangles'],
              ['--merge-materials', 'Remove redundant/duplicate materials'],
              ['--all', 'Apply all extra fixes'],
            ]}
            examples={[
              'qtmesh fix character.fbx -o clean.fbx',
              'qtmesh fix character.fbx --all',
              'qtmesh fix character.fbx --remove-degenerates --merge-materials',
            ]}
          />

          <CmdSection id="cmd-convert" name="convert" description="Convert a 3D model between formats. Output format is determined by the file extension."
            synopsis="qtmesh convert <file> -o <output>"
            options={[['-o <output>', 'Output file path (required). Extension determines format.']]}
            examples={[
              'qtmesh convert character.dae -o character.fbx',
              'qtmesh convert model.obj -o model.gltf2',
              'qtmesh convert scene.fbx -o scene.glb2',
            ]}
          >
            <h3 className={s.subsection}>Supported Formats</h3>
            <table className={s.table}>
              <thead><tr><th>Extension</th><th>Format</th><th>Import</th><th>Export</th></tr></thead>
              <tbody>
                {[
                  ['.fbx', 'FBX Binary', 'Yes', 'Yes'],
                  ['.gltf2', 'glTF 2.0', 'Yes', 'Yes'],
                  ['.glb2', 'glTF 2.0 Binary', 'Yes', 'Yes'],
                  ['.dae', 'Collada', 'Yes', 'Yes'],
                  ['.obj', 'Wavefront OBJ', 'Yes', 'Yes'],
                  ['.stl', 'STL', 'Yes', 'Yes'],
                  ['.ply', 'Stanford PLY', 'Yes', 'Yes'],
                  ['.3ds', '3D Studio', 'Yes', 'No'],
                  ['.mesh', 'Ogre Mesh', 'Yes', 'No'],
                ].map(([ext, fmt, imp, exp], i) => <tr key={i}><td><Code>{ext}</Code></td><td>{fmt}</td><td>{imp}</td><td>{exp}</td></tr>)}
              </tbody>
            </table>
          </CmdSection>

          <CmdSection id="cmd-anim" name="anim" description="List, rename, merge, resample, or decimate animations. Supports working with animation-only skeleton files."
            synopsis={`qtmesh anim <file> --list [--json]
qtmesh anim <file> --rename <old> <new> [-o <output>]
qtmesh anim <file> --merge <f1> [f2...] [-o <output>]
qtmesh anim <file> --resample N [-o <output>] [--animation <name>]
qtmesh anim <file> --decimate-step S [-o <output>] [--animation <name>]`}
            options={[
              ['--list', 'List all animations with durations'],
              ['--json', 'Output list as JSON'],
              ['--rename <old> <new>', 'Rename an animation clip'],
              ['--merge <files...>', 'Merge animations from other files into base'],
              ['--resample N', 'Resample to exactly N evenly-spaced keyframes'],
              ['--decimate-step S', 'Keep every Sth keyframe (plus first and last)'],
              ['--animation <name>', 'Target a specific animation for resample/decimate'],
              ['-o <output>', 'Output file (overwrites input if omitted)'],
            ]}
            examples={[
              'qtmesh anim character.fbx --list --json',
              'qtmesh anim character.fbx --rename "Take 001" "Idle" -o character.fbx',
              'qtmesh anim base.fbx --merge walk.fbx run.fbx jump.fbx -o merged.fbx',
              'qtmesh anim character.fbx --resample 30 --animation "Walk" -o optimized.fbx',
              'qtmesh anim character.fbx --decimate-step 5 -o lighter.fbx',
            ]}
          />

          <CmdSection id="cmd-validate" name="validate" description="Validate mesh geometry for common issues: degenerate triangles, non-finite UV coordinates, out-of-range UVs. Returns exit code 1 if errors are found."
            synopsis="qtmesh validate <file> [--json]"
            options={[['--json', 'Output validation results as JSON']]}
            examples={['qtmesh validate character.fbx', 'qtmesh validate character.fbx --json']}
          >
            <h3 className={s.subsection}>Issues Detected</h3>
            <table className={s.table}>
              <thead><tr><th>Issue</th><th>Severity</th><th>Fixable</th></tr></thead>
              <tbody>
                <tr><td>Degenerate triangles (zero area)</td><td><Badge type="error">error</Badge></td><td>Yes</td></tr>
                <tr><td>Non-finite UV coordinates (NaN/Inf)</td><td><Badge type="error">error</Badge></td><td>Yes</td></tr>
                <tr><td>Out-of-range UV values</td><td><Badge type="warning">warning</Badge></td><td>No</td></tr>
              </tbody>
            </table>
          </CmdSection>

          <CmdSection id="cmd-lod" name="lod" description="Generate, inspect, or remove Level of Detail (LOD) levels for a mesh. LODs reduce polygon count for distant rendering."
            synopsis={`qtmesh lod <file> --count N [--reductions r,...] [-o output]
qtmesh lod <file> --auto [-o output]
qtmesh lod <file> --remove [-o output]
qtmesh lod <file> --info [--json]`}
            options={[
              ['--count N', 'Generate N LOD levels'],
              ['--reductions r,...', 'Comma-separated reduction ratios (e.g. 0.25,0.5)'],
              ['--auto', 'Auto-generate LOD levels with sensible defaults'],
              ['--remove', 'Remove all LOD levels from the mesh'],
              ['--info', 'Show current LOD level information'],
              ['--json', 'Output LOD info as JSON'],
              ['-o <output>', 'Output file (overwrites input if omitted)'],
            ]}
            examples={[
              'qtmesh lod character.fbx --count 3',
              'qtmesh lod character.fbx --count 2 --reductions 0.25,0.5 -o output.fbx',
              'qtmesh lod character.fbx --auto',
              'qtmesh lod character.fbx --info --json',
              'qtmesh lod character.fbx --remove -o clean.fbx',
            ]}
          />

          <CmdSection id="cmd-pose" name="pose" description="Export a single animation frame as a static mesh, or export multiple evenly-spaced frames. Useful for generating thumbnails, sprite sheets, or static props from animated models."
            synopsis={`qtmesh pose <file> --animation <name> --time <t> -o <output>
qtmesh pose <file> --animation <name> --count N -o <pattern>`}
            options={[
              ['--animation <name>', 'Animation clip name (required)'],
              ['--time <t>', 'Time in seconds for single frame export'],
              ['--count N', 'Number of evenly-spaced frames to export'],
              ['-o <output>', 'Output file or pattern (use %02d for multi-frame)'],
            ]}
            examples={[
              'qtmesh pose character.fbx --animation "Walk" --time 0.5 -o posed.stl',
              'qtmesh pose character.fbx --animation "Dance" --count 8 -o frame_%02d.stl',
            ]}
          />

          <CmdSection id="cmd-scan" name="scan" description={<>Recursively scan a directory for 3D asset issues. Think of it as <strong>ESLint for 3D assets</strong>. Checks format restrictions, complexity limits, naming conventions, skeleton/animation content, and more. Supports YAML configuration, scoped rules per folder, JSON/SARIF output, and auto-fix.</>}
            synopsis={`qtmesh scan [path] [options]`}
            options={[
              ['--config <file>', 'Config file path (default: qtmesh.yml, qtmesh.yaml, qtmesh.json)'],
              ['--json', 'Output results as JSON'],
              ['--report <file>', 'Write JSON report to file'],
              ['--sarif <file>', 'Write SARIF report to file (for GitHub Code Scanning)'],
              ['--fix', 'Enable auto-fixes for fixable issues'],
              ['--dry-run', 'Show what fixes would be applied without making changes'],
              ['--include <patterns>', 'File patterns to include (comma-separated, e.g. *.fbx,*.glb)'],
              ['--exclude <patterns>', 'File patterns to exclude (comma-separated)'],
              ['--fail-on <level>', 'Exit code 1 threshold: info, warning, error, or never'],
            ]}
            examples={[
              'qtmesh scan ./assets',
              'qtmesh scan ./assets --config qtmesh.yml --fail-on warning',
              'qtmesh scan ./assets --json --report .qtmesh/report.json',
              'qtmesh scan ./assets --sarif report.sarif',
              'qtmesh scan ./assets --fix --dry-run',
              'qtmesh scan ./assets --include "*.fbx,*.glb" --exclude "**/vendor/**"',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`  OK    models/player.fbx
ERROR   models/enemy.fbx
         [error] max_vertex_count: 150432 vertices exceeds limit of 100000
         [error] max_material_count: 24 materials exceeds limit of 16
WARN    models/NPC_Guard.fbx
         [warning] file_name_case: Expected snake_case (suggestion: npc_guard.fbx)
ERROR   props/Barrel.dae
         [error] forbidden_extensions: .dae is a forbidden format

Summary:
  Scanned:  4
  Passed:   1
  Warnings: 1
  Errors:   4
  Time:     0.3s`}</CodeBlock>
          </CmdSection>

          {/* ─── Scan Reference ─── */}

          <section className={s.section} id="scan-config">
            <h2 className={s.sectionTitle}>Configuration (qtmesh.yml)</h2>
            <p className={s.para}>
              The scan command automatically loads <Code>qtmesh.yml</Code>, <Code>qtmesh.yaml</Code>, or <Code>qtmesh.json</Code> from the current directory. Override with <Code>--config &lt;path&gt;</Code>.
            </p>

            <h3 className={s.subsection}>Full Schema</h3>
            <CodeBlock lang="yaml">{`version: 1

scan:
  roots:                        # Directories to scan (default: current dir)
    - assets/
  include:                      # Glob patterns for files to include
    - "**/*.fbx"
    - "**/*.glb"
    - "**/*.gltf"
    - "**/*.obj"
  exclude:                      # Glob patterns for files to skip
    - "**/third_party/**"
    - "**/vendor/**"
    - "**/.git/**"

rules:
  # Format restrictions
  allowed_formats: [fbx, glb, gltf, obj]    # empty = all allowed
  forbidden_extensions: [dae, 3ds]

  # Size & complexity limits (0 = no limit)
  max_file_size_mb: 50
  min_file_size_mb: 0.001       # Catch stub/empty files
  max_mesh_count: 8
  min_mesh_count: 1
  max_material_count: 16
  min_material_count: 0
  max_vertex_count: 100000
  min_vertex_count: 3           # Catch degenerate geometry

  # Skeleton & animation existence
  require_skeleton: false
  require_animations: false

  # Animation content limits
  max_anim_keyframes: 500
  min_anim_keyframes: 2         # Catch degenerate 1-keyframe anims
  max_anim_duration: 30.0       # seconds
  min_anim_duration: 0.1        # Catch too-short anims

  # Animation & bone name requirements (wildcards: * ? )
  require_animation_names: [idle, walk, run, "attack*"]
  require_bone_names: [Hips, Spine, Head]

  # Texture & material hygiene
  allow_embedded_textures: true
  require_textures_exist: true
  allow_missing_materials: false

  # File naming convention
  file_name_case: snake_case    # snake_case|kebab-case|camelCase|PascalCase|lowercase

# Path-specific rule overrides
scopes:
  "characters/**":
    require_skeleton: true
    require_animation_names: [idle, walk, run, "attack*"]
    require_bone_names: [r_hand_attach, l_hand_attach]
  "props/**":
    max_vertex_count: 5000
    require_skeleton: false

fix:
  enabled: false
  dry_run: false
  optimize_meshes: false
  rename_animations: false
  convert_to_format: ""
  output_dir: ""

report:
  format: both                  # text | json | both
  output: .qtmesh/scan-report.json
  sarif_output: .qtmesh/scan-report.sarif
  fail_on: error                # info | warning | error | never`}</CodeBlock>
          </section>

          <section className={s.section} id="scan-rules">
            <h2 className={s.sectionTitle}>Rules Reference</h2>
            <p className={s.para}>
              All rules are optional. Set to <Code>0</Code> (numeric), <Code>false</Code> (boolean), or empty (lists) to disable.
              Max rules emit <Badge type="error">error</Badge>. Min rules emit <Badge type="warning">warning</Badge>.
            </p>

            <h3 className={s.subsection}>Format Rules</h3>
            <RuleCard name="allowed_formats" type="string[]" severity="error"
              description="Restrict which file formats are accepted. Empty list allows all formats."
              example={`allowed_formats: [fbx, glb, gltf, obj]`} />
            <RuleCard name="forbidden_extensions" type="string[]" severity="error"
              description="Explicitly forbid specific formats."
              example={`forbidden_extensions: [dae, 3ds, stl]`} />

            <h3 className={s.subsection}>Size & Complexity Rules</h3>
            {[
              ['max_file_size_mb', 'number', 'error', 'Maximum file size in megabytes.', 'max_file_size_mb: 50'],
              ['min_file_size_mb', 'number', 'warning', 'Minimum file size in MB. Catches stub or corrupted files.', 'min_file_size_mb: 0.001'],
              ['max_mesh_count', 'number', 'error', 'Maximum number of meshes (submeshes) per asset.', 'max_mesh_count: 8'],
              ['min_mesh_count', 'number', 'warning', 'Minimum mesh count. Catches empty geometry containers.', 'min_mesh_count: 1'],
              ['max_material_count', 'number', 'error', 'Maximum number of materials per asset.', 'max_material_count: 16'],
              ['min_material_count', 'number', 'warning', 'Minimum material count.', 'min_material_count: 1'],
              ['max_vertex_count', 'number', 'error', 'Maximum total vertex count across all meshes.', 'max_vertex_count: 100000'],
              ['min_vertex_count', 'number', 'warning', 'Minimum vertex count. Catches degenerate geometry.', 'min_vertex_count: 3'],
            ].map(([name, type, sev, desc, ex]) => (
              <RuleCard key={name} name={name} type={type} severity={sev} description={desc} example={ex} />
            ))}

            <h3 className={s.subsection}>Skeleton & Animation Existence</h3>
            <RuleCard name="require_skeleton" type="boolean" severity="error"
              description="Require a skeleton (at least one bone) to be present."
              example="require_skeleton: true" />
            <RuleCard name="require_animations" type="boolean" severity="error"
              description="Require at least one animation clip to be present."
              example="require_animations: true" />

            <h3 className={s.subsection}>Animation Content Rules</h3>
            {[
              ['max_anim_keyframes', 'number', 'error', 'Maximum keyframes per animation. Checked per-clip.', 'max_anim_keyframes: 500'],
              ['min_anim_keyframes', 'number', 'warning', 'Minimum keyframes per animation. Catches degenerate single-keyframe clips.', 'min_anim_keyframes: 2'],
              ['max_anim_duration', 'number', 'error', 'Maximum animation duration in seconds. Checked per-clip.', 'max_anim_duration: 30.0'],
              ['min_anim_duration', 'number', 'warning', 'Minimum animation duration in seconds.', 'min_anim_duration: 0.1'],
            ].map(([name, type, sev, desc, ex]) => (
              <RuleCard key={name} name={name} type={type} severity={sev} description={desc} example={ex} />
            ))}

            <RuleCard name="require_animation_names" type="string[]" severity="error"
              description={<>Require specific animation clips to be present. Supports wildcards: <Code>*</Code> matches any string, <Code>?</Code> matches one character. Case-insensitive.</>}
              example={`require_animation_names:\n  - idle\n  - walk\n  - run\n  - "attack*"     # matches Attack1, attack_heavy, etc.\n  - "dance_?"     # matches dance_1, dance_A, etc.`} />

            <RuleCard name="require_bone_names" type="string[]" severity="error"
              description={<>Require specific bones to be present in the skeleton. Same wildcard support as animation names.</>}
              example={`require_bone_names:\n  - Hips\n  - Spine\n  - Head\n  - r_hand_attach\n  - l_hand_attach\n  - "weapon_slot_*"`} />

            <h3 className={s.subsection}>Texture & Material Rules</h3>
            <RuleCard name="allow_embedded_textures" type="boolean" severity="warning"
              description="Set to false to warn when assets contain embedded textures."
              example="allow_embedded_textures: false" />
            <RuleCard name="require_textures_exist" type="boolean" severity="warning"
              description="Verify that referenced texture files exist on disk relative to the asset."
              example="require_textures_exist: true" />
            <RuleCard name="allow_missing_materials" type="boolean" severity="warning"
              description="Set to false to warn about placeholder materials (DefaultMaterial, AI_DEFAULT, etc.)."
              example="allow_missing_materials: false" />

            <h3 className={s.subsection}>Naming Rules</h3>
            <RuleCard name="file_name_case" type="string" severity="warning"
              description={<>Enforce file naming convention. Supported: <Code>snake_case</Code>, <Code>kebab-case</Code>, <Code>camelCase</Code>, <Code>PascalCase</Code>, <Code>lowercase</Code>. This rule is auto-fixable.</>}
              example="file_name_case: snake_case" />
          </section>

          <section className={s.section} id="scan-scopes">
            <h2 className={s.sectionTitle}>Scoped Validation</h2>
            <p className={s.para}>
              Scopes let you apply different rules to different directory paths. This is essential for game projects where characters, props, and environments have different requirements.
            </p>
            <p className={s.para}>
              Global <Code>rules:</Code> apply to all assets by default. Each scope matches a glob pattern against the asset's relative path and overrides specific rule fields.
              Multiple scopes can match the same file; they apply in order (later overrides earlier).
            </p>

            <h3 className={s.subsection}>Example: Game Asset Pipeline</h3>
            <CodeBlock lang="yaml">{`rules:
  max_vertex_count: 100000
  file_name_case: snake_case

scopes:
  "characters/**":
    require_skeleton: true
    require_animations: true
    max_anim_keyframes: 300
    require_animation_names:
      - idle
      - walk
      - run
      - jump
      - "attack*"
      - "dance_*"
    require_bone_names:
      - r_hand_attach
      - l_hand_attach
      - backpack
      - top_head
  "props/**":
    require_skeleton: false
    max_vertex_count: 5000
    max_material_count: 4
  "environments/**":
    max_vertex_count: 500000
    max_material_count: 32
  "ui/**":
    max_vertex_count: 1000
    max_mesh_count: 1`}</CodeBlock>
          </section>

          <section className={s.section} id="scan-output">
            <h2 className={s.sectionTitle}>Output Formats</h2>
            <h3 className={s.subsection}>Text (default)</h3>
            <p className={s.para}>Human-readable terminal output with per-asset status and findings.</p>

            <h3 className={s.subsection}>JSON (<Code>--json</Code> or <Code>--report &lt;file&gt;</Code>)</h3>
            <p className={s.para}>Machine-readable output with full asset metadata and findings.</p>
            <CodeBlock>{`{
  "version": "2.x.x",
  "summary": {
    "scanned": 5, "passed": 3, "warnings": 1,
    "errors": 2, "fixed": 0, "skipped": 0
  },
  "assets": [{
    "file": "characters/hero.fbx",
    "format": "fbx",
    "fileSize": 2345678,
    "meshCount": 3,
    "materialCount": 5,
    "vertexCount": 45000,
    "faceCount": 22000,
    "animationCount": 12,
    "hasSkeleton": true,
    "boneCount": 65,
    "animations": [
      { "name": "Walk", "duration": 1.0, "keyframes": 30 },
      { "name": "Run", "duration": 0.8, "keyframes": 24 }
    ],
    "bones": ["Hips", "Spine", "Head", "..."],
    "findings": []
  }]
}`}</CodeBlock>

            <h3 className={s.subsection}>SARIF (<Code>--sarif &lt;file&gt;</Code>)</h3>
            <p className={s.para}>
              <a href="https://sarifweb.azurewebsites.net/" target="_blank" rel="noopener" style={{color: 'var(--accent-1)'}}>SARIF 2.1.0</a> format
              for integration with GitHub Code Scanning, VS Code, and other analysis tools.
            </p>
            <CodeBlock>{`# Upload SARIF to GitHub Code Scanning
- uses: github/codeql-action/upload-sarif@v3
  with:
    sarif_file: scan-report.sarif`}</CodeBlock>
          </section>

          <section className={s.section} id="scan-fix">
            <h2 className={s.sectionTitle}>Auto-Fix</h2>
            <p className={s.para}>
              Some rules support automatic fixes. Use <Code>--fix</Code> to apply them, or <Code>--fix --dry-run</Code> to preview without changes.
            </p>
            <table className={s.table}>
              <thead><tr><th>Rule</th><th>Fix Action</th></tr></thead>
              <tbody>
                <tr><td><Code>file_name_case</Code></td><td>Renames the file to match the convention</td></tr>
              </tbody>
            </table>
            <p className={s.para}>
              Additional fix capabilities (mesh optimization, format conversion) are planned. Use <Code>qtmesh fix</Code> for per-file mesh optimization.
            </p>
          </section>

          {/* ─── Integration ─── */}

          <section className={s.section} id="docker">
            <h2 className={s.sectionTitle}>Docker</h2>
            <p className={s.para}>All CLI commands work via Docker. The image includes a headless OpenGL environment for Ogre-based operations.</p>
            <CodeBlock>{`# Inspect a model
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh \\
  info model.fbx --json

# Convert formats
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh \\
  convert model.fbx -o model.gltf2

# Scan a directory
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh \\
  scan /workspace/assets --config /workspace/qtmesh.yml --json

# Validate geometry
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh \\
  validate model.fbx`}</CodeBlock>
          </section>

          <section className={s.section} id="github-actions">
            <h2 className={s.sectionTitle}>GitHub Actions</h2>
            <h3 className={s.subsection}>Reusable Action (scan example)</h3>
            <CodeBlock lang="yaml">{`- uses: fernandotonon/QtMeshEditor/.github/actions/qtmesh@9cfc829e8b255994ef92ba228c687e3dd2254119
  with:
    command: scan
    input-file: assets
    options: --config qtmesh.yml --sarif scan-report.sarif --report scan-report.json --fail-on error`}</CodeBlock>

            <h3 className={s.subsection}>Direct Docker Usage</h3>
            <CodeBlock lang="yaml">{`- name: Scan assets
  run: |
    docker run --rm \\
      -v \${{ github.workspace }}:/workspace \\
      ghcr.io/fernandotonon/qtmesh:latest \\
      scan /workspace/assets \\
      --config /workspace/qtmesh.yml \\
      --sarif /workspace/scan-report.sarif \\
      --fail-on error

- name: Upload SARIF to GitHub
  if: always()
  uses: github/codeql-action/upload-sarif@v3
  with:
    sarif_file: scan-report.sarif`}</CodeBlock>
          </section>

          <section className={s.section} id="gitlab-ci">
            <h2 className={s.sectionTitle}>GitLab CI</h2>
            <p className={s.para}>
              Use the Docker image directly in <Code>.gitlab-ci.yml</Code> to run scan checks and keep reports as artifacts.
            </p>
            <CodeBlock lang="yaml">{`stages:
  - lint

asset_scan:
  stage: lint
  image: ghcr.io/fernandotonon/qtmesh:2.23.0
  entrypoint: [""]
  script:
    - qtmesheditor --cli scan \${CI_PROJECT_DIR}/assets \\
        --config \${CI_PROJECT_DIR}/qtmesh.yml \\
        --sarif \${CI_PROJECT_DIR}/scan-report.sarif \\
        --report \${CI_PROJECT_DIR}/scan-report.json \\
        --fail-on error
  artifacts:
    when: always
    paths:
      - scan-report.sarif
      - scan-report.json
    expire_in: 7 days`}</CodeBlock>
          </section>

          <section className={s.section} id="ci-cd">
            <h2 className={s.sectionTitle}>CI/CD Patterns</h2>
            <h3 className={s.subsection}>PR Gate: Block Merges on Asset Errors</h3>
            <CodeBlock lang="yaml">{`name: Asset Lint
on: [pull_request]
jobs:
  scan:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Scan assets
        run: |
          docker run --rm -v \${{ github.workspace }}:/workspace \\
            ghcr.io/fernandotonon/qtmesh:latest \\
            scan /workspace/assets --fail-on error
      # Exit code 1 blocks the PR`}</CodeBlock>

            <h3 className={s.subsection}>Nightly Report: Warnings as Annotations</h3>
            <CodeBlock lang="yaml">{`name: Nightly Asset Report
on:
  schedule:
    - cron: '0 6 * * *'
jobs:
  report:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Scan with SARIF
        run: |
          docker run --rm -v \${{ github.workspace }}:/workspace \\
            ghcr.io/fernandotonon/qtmesh:latest \\
            scan /workspace/assets \\
            --sarif /workspace/report.sarif \\
            --fail-on never
      - uses: github/codeql-action/upload-sarif@v3
        with:
          sarif_file: report.sarif`}</CodeBlock>

            <h3 className={s.subsection}>Pre-commit Hook</h3>
            <CodeBlock>{`#!/bin/sh
# .git/hooks/pre-commit
# Scan only staged 3D files
STAGED=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\\.(fbx|glb|gltf|obj)$')
if [ -n "$STAGED" ]; then
  qtmesh scan . --include "$(echo $STAGED | tr ' ' ',')" --fail-on error
fi`}</CodeBlock>
          </section>

          <footer style={{ borderTop: '1px solid var(--stroke)', padding: '2rem 0', marginTop: '3rem', color: 'var(--text-muted)', fontSize: '0.85rem', textAlign: 'center' }}>
            <a href="./index.html" style={{ color: 'var(--accent-1)', textDecoration: 'none' }}>QtMeshEditor</a>
            {' '}&middot;{' '}
            <a href="https://github.com/fernandotonon/QtMeshEditor" target="_blank" rel="noopener" style={{ color: 'var(--accent-1)', textDecoration: 'none' }}>GitHub</a>
            {' '}&middot;{' '}
            <a href="https://github.com/fernandotonon/QtMeshEditor/releases" target="_blank" rel="noopener" style={{ color: 'var(--accent-1)', textDecoration: 'none' }}>Releases</a>
          </footer>

        </div>
      </main>
    </div>
  );
}
