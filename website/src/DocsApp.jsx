import React, { useState, useEffect } from 'react';
import s from './DocsApp.module.css';
import useQtmeshActionRef from './hooks/useQtmeshActionRef';

const NAV = [
  { section: 'Getting Started', items: [
    { id: 'installation', label: 'Installation' },
    { id: 'file-associations', label: 'Open 3D Files' },
    { id: 'quick-start', label: 'Quick Start' },
    { id: 'playstation-rsd-ply', label: 'PlayStation RSD / Psy-Q PLY' },
  ]},
  { section: 'CLI Commands', items: [
    { id: 'cmd-info', label: 'info' },
    { id: 'cmd-fix', label: 'fix' },
    { id: 'cmd-convert', label: 'convert' },
    { id: 'cmd-anim', label: 'anim' },
    { id: 'cmd-validate', label: 'validate' },
    { id: 'cmd-lod', label: 'lod' },
    { id: 'cmd-pose', label: 'pose' },
    { id: 'cmd-turntable', label: 'turntable' },
    { id: 'cmd-isometric', label: 'isometric' },
    { id: 'cmd-vat', label: 'vat' },
    { id: 'cmd-scan', label: 'scan' },
  ]},
  { section: 'Performance', items: [
    { id: 'perf-overview', label: 'Concepts' },
    { id: 'cmd-memory', label: 'memory' },
    { id: 'cmd-analyze', label: 'analyze' },
    { id: 'cmd-vertex-cache', label: 'vertex-cache' },
    { id: 'cmd-decimate', label: 'decimate' },
    { id: 'cmd-atlas', label: 'atlas' },
    { id: 'cmd-optimize', label: 'optimize' },
    { id: 'cmd-uv', label: 'uv' },
  ]},
  { section: 'Scan Reference', items: [
    { id: 'scan-config', label: 'Configuration (qtmesh.yml)' },
    { id: 'scan-profiles', label: 'Platform Profiles' },
    { id: 'scan-rules', label: 'Rules Reference' },
    { id: 'scan-scopes', label: 'Scoped Validation' },
    { id: 'scan-output', label: 'Output Formats' },
    { id: 'scan-fix', label: 'Auto-Fix' },
  ]},
  { section: 'Integration', items: [
    { id: 'qtmesh-cloud', label: 'QtMesh Cloud Badges' },
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
  const { actionRef: qtmeshActionRef, imageTag: qtmeshImageTag } = useQtmeshActionRef();

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
brew trust fernandotonon/qtmesheditor   # Homebrew now requires trusting third-party taps
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

          <section className={s.section} id="file-associations">
            <h2 className={s.sectionTitle}>Open 3D Files from the OS</h2>
            <p className={s.para}>
              QtMeshEditor registers as a document handler on macOS, Windows, and Linux. After install,
              double-click supported models or use your file manager&apos;s <strong>Open With</strong> menu.
              Launching with a file path loads it in the GUI; if the app is already running, the existing
              window is focused and the new file is imported (single-instance).
            </p>
            <h3 className={s.subsection}>App import support</h3>
            <p className={s.para}>
              The editor can import many formats via drag-and-drop or <Code>QtMeshEditor path/to/model.ext</Code>,
              including <Code>.vrm</Code>, <Code>.3ds</Code>, and additional Assimp types beyond the OS registration
              sets below.
            </p>
            <h3 className={s.subsection}>Registered Open With handlers</h3>
            <ul className={s.para} style={{ paddingLeft: '1.4rem' }}>
              <li><strong>macOS</strong> — <Code>.fbx</Code>, <Code>.glb</Code>/<Code>.gltf</Code>, <Code>.obj</Code>,
                  <Code>.dae</Code>, <Code>.stl</Code>, <Code>.ply</Code>, <Code>.vrm</Code>, <Code>.3ds</Code>,
                  <Code>.scene.glb</Code>/<Code>.scene.gltf</Code>, <Code>.mesh</Code>, <Code>.rsd</Code>, <Code>.tmd</Code>
                  via <Code>CFBundleDocumentTypes</Code>; Homebrew cask runs <Code>lsregister</Code> after install.</li>
              <li><strong>Windows</strong> — <Code>.fbx</Code>, <Code>.glb</Code>, <Code>.gltf</Code>, <Code>.obj</Code>,
                  <Code>.dae</Code>, <Code>.stl</Code>, <Code>.ply</Code>, <Code>.mesh</Code>, <Code>.rsd</Code>, <Code>.tmd</Code>
                  added to <Code>OpenWithProgids</Code> (not default-app takeover). Inno Setup installer on release;
                  portable ZIP includes <Code>bin/scripts/register-windows-file-associations.ps1</Code>.</li>
              <li><strong>Linux</strong> — <Code>.fbx</Code>, <Code>.glb</Code>/<Code>.gltf</Code>, <Code>.obj</Code>,
                  <Code>.dae</Code>, <Code>.stl</Code>, <Code>.ply</Code>, <Code>.mesh</Code>, <Code>.rsd</Code>, <Code>.tmd</Code>,
                  <Code>.scene.glb</Code>/<Code>.scene.gltf</Code> via <Code>.desktop</Code> + XDG MIME in
                  <Code>.deb</Code> and Snap; postinstall runs <Code>update-mime-database</Code> and
                  <Code>update-desktop-database</Code>.</li>
            </ul>
            <p className={s.para}>
              CLI mode is unchanged: <Code>qtmesh info model.fbx</Code> still runs headless. Only
              <Code>QtMeshEditor model.fbx</Code> (or OS open-with) enters GUI-with-file mode.
            </p>
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
qtmesh scan ./assets --fail-on error

# Render a product-shot turntable PNG
qtmesh turntable character.fbx -o preview.png --frames 12 --size 512`}</CodeBlock>

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

          <section className={s.section} id="playstation-rsd-ply">
            <h2 className={s.sectionTitle}>PlayStation RSD, Psy-Q PLY, and MAT</h2>
            <p className={s.para}>
              <strong>RSD</strong> (<Code>.rsd</Code>) is a PlayStation-era descriptor that references a mesh (often <Code>.tmd</Code> or Psy-Q <Code>.ply</Code>) plus optional <Code>.tim</Code> textures and <Code>.mat</Code> sidecars.
              Import resolves those paths, loads geometry, and applies TIM-driven materials when possible. Export writes the <Code>.rsd</Code> and companion filenames next to the output.
            </p>
            <p className={s.para}>
              <strong>Psy-Q PLY</strong> is not Stanford PLY: it uses an <Code>@PLY…</Code> header, separate <strong>vertex</strong> and <strong>normal</strong> count lines (<Code>nV nN nF</Code>), then face lines where type <Code>0</Code> is a triangle and <Code>1</Code> is a quad with independent v/n indices.
              Quads use the classic split <Code>(v0,v1,v2)</Code> + <Code>(v1,v2,v3)</Code> when expanded to triangles in the engine.
            </p>
            <p className={s.para}>
              <strong>Import</strong> welds corners (position + normal ± colour) and stores quad/ngon topology as <Code>qtme.faces.*</Code> when the file encodes it, so artist intent is preserved.
            </p>
            <p className={s.para}>
              <strong>Export</strong> writes welded <strong>position</strong> and <strong>normal</strong> pools separately (quantized floats). Many corners share one quantized normal, so <Code>nN</Code> is often <strong>smaller than</strong> <Code>nV</Code> and smaller than in a verbose original — that is deduplication, not loss of shading, for flat or smooth regions.
            </p>
            <p className={s.para}>
              If <Code>qtme.faces</Code> exists, face lines follow those polygons. Otherwise coplanar triangle pairs matching the PS1 adjacency pattern with nearly parallel normals (dot ≥ 0.94) are <strong>merged back into quads</strong>, which often recovers cleaner layouts than triangle-only dumps.
            </p>
            <p className={s.para}>
              Full write-up in the repository:{' '}
              <a href="https://github.com/fernandotonon/QtMeshEditor/blob/master/documentation/playstation-rsd-ply.md" target="_blank" rel="noreferrer">documentation/playstation-rsd-ply.md</a>.
            </p>
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
                  ['.ply', 'Stanford PLY or Psy-Q PLY (by content)', 'Yes', 'Yes'],
                  ['.tmd', 'PlayStation TMD', 'Yes', 'Yes'],
                  ['.rsd', 'PlayStation RSD (descriptor + sidecars)', 'Yes', 'Yes'],
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

          <CmdSection id="cmd-turntable" name="turntable" description={<>Headless render-to-texture turntable for mesh previews. Orbits a camera around the model&apos;s bounding box and writes PNG frames — either a single horizontal <strong>sprite sheet</strong> (default for multiple frames) or separate files when <Code>-o</Code> contains a frame index pattern such as <Code>frame_%02d.png</Code>. Uses the same Ogre RTSS material path as the editor viewport (normal maps via <Code>SRS_NORMALMAP</Code>, not as a flat FFP overlay). Supports FBX, glTF, OBJ, and other formats the editor imports.</>}
            synopsis={`qtmesh turntable <file> -o <output> [--frames N] [--size WxH] [--columns C]
qtmesh turntable <file> -o frame_%02d.png [--frames N] [--axis y|x|z]`}
            options={[
              ['-o <output>', 'Output PNG path. One file for a single frame or sprite sheet; use exactly one %d-style index (e.g. frame_%02d.png) for a numbered sequence.'],
              ['--frames N', 'Number of views around the orbit (1–360, default 12)'],
              ['--size WxH', 'Frame width and height (default 512×512). A single number sets a square size.'],
              ['--width N / --height N', 'Override width or height independently'],
              ['--columns C', 'Sprite-sheet columns (0 = one row, default for multi-frame)'],
              ['--axis y|x|z', 'Orbit axis: Y = XZ turntable (default), X = YZ, Z = XY'],
              ['--elevation <deg>', 'Camera angle above the orbit plane in degrees (default 20)'],
              ['--camera-height <deg>', 'Alias for --elevation'],
              ['--json', 'Emit paths and settings as JSON'],
            ]}
            examples={[
              'qtmesh turntable character.fbx -o preview.png --frames 12 --size 512',
              'qtmesh turntable prop.glb -o sheet.png --frames 8 --columns 4 --size 256',
              'qtmesh turntable character.fbx -o frames/frame_%02d.png --frames 24 --axis y --elevation 15',
              'qtmesh turntable character.fbx -o thumb.png --frames 1 --size 128 --json',
            ]}
          >
            <h3 className={s.subsection}>Output modes</h3>
            <table className={s.table}>
              <thead><tr><th>Frames</th><th><Code>-o</Code> pattern</th><th>Result</th></tr></thead>
              <tbody>
                <tr><td><Code>1</Code></td><td><Code>preview.png</Code></td><td>Single PNG</td></tr>
                <tr><td><Code>N &gt; 1</Code></td><td><Code>sheet.png</Code> (no <Code>%d</Code>)</td><td>One horizontal sprite sheet (<Code>N</Code> tiles in a row unless <Code>--columns</Code> is set)</td></tr>
                <tr><td><Code>N &gt; 1</Code></td><td><Code>frame_%02d.png</Code></td><td><Code>N</Code> separate PNGs (<Code>frame_00.png</Code> …)</td></tr>
              </tbody>
            </table>
            <p className={s.para}>
              Sequence patterns must contain <strong>exactly one</strong> integer conversion (<Code>%d</Code>, <Code>%02d</Code>, etc.).
              Use <Code>%%</Code> in the path for a literal percent sign. Invalid numeric flags (e.g. <Code>--frames abc</Code>) return exit code <Code>2</Code>.
            </p>
          </CmdSection>

          <CmdSection id="cmd-isometric" name="isometric" description={<>Headless render-to-texture export for <strong>isometric / 8-direction</strong> sprite atlases used by 2D games (Godot <Code>AnimatedSprite2D</Code>, Unity sliced sheets, etc.). Renders the model from fixed compass directions while optionally sampling an animation into evenly spaced frames. Output is a single PNG grid: <strong>rows = directions</strong>, <strong>columns = animation frames</strong>. Row 0 is the front view (camera on +Z); each subsequent row rotates clockwise when viewed from above (+Y). Uses the same Ogre RTSS path as <Code>turntable</Code>.</>}
            synopsis={`qtmesh isometric <file> -o <output.png> [--directions N] [--frames N] [--animation NAME]`}
            options={[
              ['-o <output>', 'Output PNG path (required)'],
              ['--directions N', 'Compass direction rows (default 8; clamped 1–64)'],
              ['--frames N', 'Animation frame columns (default 1 static, 8 when --animation is set)'],
              ['--animation NAME', 'Sample this skeletal animation across --frames columns'],
              ['--elevation DEG', 'Camera elevation above the orbit plane (default 30; alias --camera-height)'],
              ['--start-azimuth DEG', 'Rotate row 0 to match your game facing (default 0)'],
              ['--resolution N', 'Square per-cell resolution in pixels (default 512; range 16–8192)'],
              ['--size WxH', 'Per-cell resolution (default 512×512)'],
              ['--width W / --height H', 'Per-cell dimensions (override --resolution)'],
              ['--json', 'Emit machine-readable report (grid dims, direction order, paths)'],
            ]}
            examples={[
              'qtmesh isometric character.fbx -o iso.png',
              'qtmesh isometric character.fbx --animation "Walk" --frames 8 -o iso_walk.png --resolution 256',
              'qtmesh isometric prop.glb -o iso_prop.png --directions 4 --resolution 128 --json',
            ]}
          />

          <CmdSection id="cmd-vat" name="vat" description={<>Bake a skeletal animation to a <strong>Vertex Animation Texture</strong> (OpenVAT format). The output is a 16-bit PNG storing per-vertex position+normal samples in time, a JSON sidecar with the playback bounds, a vertex-order-aligned glTF, and an Ogre bind sidecar (<Code>{`<basename>_ogre_bind.bin`}</Code>) so engine importers can realign UV2 to the bake's column order on import. Drop-in shader templates for Godot/Unity/Unreal ship at <a href="https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/vat-shaders" className={s.link}>tools/vat-shaders/</a> and can be copied next to the bake with <Code>--include-shaders</Code>. Live demos: <a href="/#vat-demo" className={s.link}>Godot (web)</a>, <a href="https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/unity-vat-demo" className={s.link}>Unity sample project</a>, <a href="https://github.com/fernandotonon/QtMeshEditor/tree/master/tools/unreal-vat-demo" className={s.link}>Unreal sample project</a>.</>}
            synopsis={`qtmesh vat <file> --anim <name> [--fps N] [-o <dir>] [--include-shaders <list>] [--json]`}
            options={[
              ['--anim <name>', 'Animation clip name to bake (required)'],
              ['--fps N', 'Frames per second to sample at (default: 30)'],
              ['-o <dir>', 'Output directory (default: <input>_vat alongside the source)'],
              ['--include-shaders <list>', 'Copy drop-in shader templates next to the bake. Comma-separated subset of godot, unity, unreal, or "all"'],
              ['--json', 'Emit a machine-readable summary instead of human text'],
            ]}
            examples={[
              'qtmesh vat character.fbx --anim "Run" -o bakes/run/',
              'qtmesh vat character.fbx --anim "Idle" --fps 24 -o bakes/idle/',
              'qtmesh vat character.fbx --anim "Dance" --include-shaders godot,unity',
              'qtmesh vat character.fbx --anim "Walk" --include-shaders all --json',
            ]}
          />

          <CmdSection id="cmd-scan" name="scan" description={<>Recursively scan a directory for 3D asset issues. Think of it as <strong>ESLint for 3D assets</strong>. Checks format restrictions, complexity limits, naming conventions, skeleton/animation content, and more. Supports YAML configuration, scoped rules per folder, JSON output, and auto-fix. Also available as a <a href="https://github.com/marketplace/actions/qtmesheditor" className={s.link}>GitHub Action</a>: <Code>{qtmeshActionRef}</Code>.</>}
            synopsis={`qtmesh scan [path] [options]`}
            options={[
              ['--config <file>', 'Config file path (default: qtmesh.yml, qtmesh.yaml, qtmesh.json)'],
              ['--target <id>', 'Built-in platform profile id (alias for --profile; CI-friendly)'],
              ['--profile <id>', 'Built-in profile id (e.g. modern-console) or path to a custom .json profile'],
              ['--list-profiles', 'List bundled platform profile ids and exit'],
              ['--json', 'Output results as JSON'],
              ['--report <file>', 'Write JSON report to file'],
              ['--sarif <file>', 'Write SARIF report to file (for GitHub Code Scanning)'],
              ['--fix', 'Enable auto-fixes for fixable issues'],
              ['--dry-run', 'Show what fixes would be applied without making changes'],
              ['--include <patterns>', 'File patterns to include (comma-separated, e.g. *.fbx,*.glb)'],
              ['--exclude <patterns>', 'File patterns to exclude (comma-separated)'],
              ['--allowed-formats <list>', 'Allowed formats CSV (e.g. fbx,glb,obj)'],
              ['--forbidden-extensions <list>', 'Forbidden formats CSV (e.g. dae,3ds)'],
              ['--max-file-size-mb <n>', 'Override max_file_size_mb for this run (0 = no limit)'],
              ['--min-file-size-mb <n>', 'Override min_file_size_mb for this run (0 = no limit)'],
              ['--max-meshes <n>', 'Override max_mesh_count for this run (0 = no limit)'],
              ['--min-meshes <n>', 'Override min_mesh_count for this run (0 = no limit)'],
              ['--max-materials <n>', 'Override max_material_count for this run (0 = no limit)'],
              ['--min-materials <n>', 'Override min_material_count for this run (0 = no limit)'],
              ['--max-vertices <n>', 'Override max_vertex_count for this run (0 = no limit)'],
              ['--min-vertices <n>', 'Override min_vertex_count for this run (0 = no limit)'],
              ['--max-acmr <n>', 'Override max_acmr for this run (e.g. 1.5; 0 = no limit)'],
              ['--require-skeleton / --no-require-skeleton', 'Enable/disable require_skeleton for this run'],
              ['--require-animations / --no-require-animations', 'Enable/disable require_animations for this run'],
              ['--allow-embedded-textures / --disallow-embedded-textures', 'Enable/disable embedded textures for this run'],
              ['--require-textures-exist / --no-require-textures-exist', 'Enable/disable texture existence checks'],
              ['--allow-missing-materials / --disallow-missing-materials', 'Enable/disable missing material checks'],
              ['--file-name-case <name>', 'snake_case, kebab-case, camelCase, PascalCase, lowercase'],
              ['--max-anim-keyframes <n>', 'Override max_anim_keyframes for this run (0 = no limit)'],
              ['--min-anim-keyframes <n>', 'Override min_anim_keyframes for this run (0 = no limit)'],
              ['--max-anim-duration <n>', 'Override max_anim_duration in seconds (0 = no limit)'],
              ['--min-anim-duration <n>', 'Override min_anim_duration in seconds (0 = no limit)'],
              ['--require-animation-names <list>', 'Required animation names/patterns CSV'],
              ['--require-bone-names <list>', 'Required bone names/patterns CSV'],
              ['--fail-on <level>', 'Exit code 1 threshold: info, warning, error, or never'],
            ]}
            examples={[
              'qtmesh scan ./assets',
              'qtmesh scan ./assets --target modern-console --fail-on warning',
              'qtmesh scan ./assets --list-profiles',
              'qtmesh scan ./assets --config qtmesh.yml --fail-on warning',
              'qtmesh scan ./assets --json --report report.json',
              'qtmesh scan ./assets --fix --dry-run',
              'qtmesh scan ./assets --include "*.fbx,*.glb" --exclude "**/vendor/**"',
              'qtmesh scan ./assets --max-vertices 120000 --max-materials=16',
              'qtmesh scan ./assets --no-require-skeleton --allowed-formats fbx,glb,obj',
              'qtmesh scan ./assets --require-animation-names "walk,run,idle*"',
            ]}
          >
            <p className={s.para}>
              Most value flags accept both styles: <Code>--flag value</Code> and <Code>--flag=value</Code>.
              CLI overrides are applied after loading <Code>qtmesh.yml</Code>/<Code>qtmesh.yaml</Code>/<Code>qtmesh.json</Code> for quick one-off validation checks.
              Bundled <a href="#scan-profiles" className={s.link}>platform profiles</a> apply preset budgets via
              <Code>--target</Code> / <Code>--profile</Code> or <Code>profile:</Code> in YAML.
            </p>
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
  • Scanned:  4
  ✓ Passed:   1
  ▲ Warnings: 1
  ✗ Errors:   3
  ⏱ Time:     0.3s`}</CodeBlock>
          </CmdSection>

          {/* ─── Performance ─── */}

          <section className={s.section} id="perf-overview">
            <h2 className={s.sectionTitle}>Performance Concepts</h2>
            <p className={s.para}>
              QtMeshEditor ships three first-class performance analyses, all accessible from the
              CLI, the MCP server, and the Inspector's <Code>Run Validation</Code> checklist.
              They never modify your source files unless you explicitly ask — analysis is
              read-only by default, rewrites are opt-in.
            </p>

            <h3 className={s.subsection}>GPU memory (VRAM)</h3>
            <p className={s.para}>
              The bytes a mesh occupies on the GPU once it's resident, broken into two pieces:
            </p>
            <ul className={s.para} style={{ paddingLeft: '1.4rem' }}>
              <li><strong>Mesh bytes</strong>: <Code>vertexCount × stride + indexCount × indexSize</Code>.
                  Stride is determined by the vertex declaration (positions + normals + UVs + bone weights, etc.).
                  Index size is 2 bytes for 16-bit buffers and 4 bytes for 32-bit.</li>
              <li><strong>Texture bytes</strong>: <Code>width × height × bytesPerPixel</Code>, multiplied by
                  <Code>4/3</Code> when a mipmap chain is present (the chain converges to a third of the base).</li>
            </ul>
            <p className={s.para}>
              The validator's <Code>GPU:</Code> row sums both, deduplicating shared meshes so totals reflect
              actual GPU residents (not draw-call counts). <Code>qtmesh memory</Code> exposes the same numbers
              from a file path. The <Code>--budget</Code> flag (or the <Code>memory_budget_mb</Code> rule on a
              QtMesh Cloud project) flips the command's exit code to <Code>1</Code> when the scene exceeds the
              configured ceiling — useful as a CI gate.
            </p>

            <h3 className={s.subsection}>Draw calls</h3>
            <p className={s.para}>
              One draw call per <Code>SubEntity</Code>. Ogre cannot batch SubEntities that share a material
              into a single draw call automatically, so two cubes with the same wood material still cost two
              calls. <Code>qtmesh analyze</Code> groups every loaded entity by the materials its SubEntities
              use and reports two numbers:
            </p>
            <ul className={s.para} style={{ paddingLeft: '1.4rem' }}>
              <li><strong>Draw calls</strong>: today's cost.</li>
              <li><strong>After merges</strong>: the cost if all <em>N</em> entities sharing a material were
                  combined into one — saving <em>N−1</em> calls per cluster.</li>
            </ul>
            <p className={s.para}>
              The validator surfaces this as a <Code>Draws:</Code> info row with the merge-savings count.
              The merge itself isn't done from the validator yet (it's a write op crossing the undo stack);
              the suggestion in the JSON tells you which entities to combine in Edit Mode or via a future
              <Code>qtmesh optimize</Code> command.
            </p>

            <h3 className={s.subsection}>Decimation (poly reduction)</h3>
            <p className={s.para}>
              Cutting an asset's triangle count is two very different things in this tool:
            </p>
            <ul className={s.para} style={{ paddingLeft: '1.4rem' }}>
              <li><strong><Code>qtmesh lod</Code></strong> generates a <em>chain</em> of LOD
                  levels (LOD 0/1/2/3) for distance-based rendering. The original mesh is the
                  base, the reduced versions sit alongside as additional levels.</li>
              <li><strong><Code>qtmesh decimate</Code></strong> performs a <em>single-pass</em>
                  reduction that rewrites the base mesh itself. There's no LOD chain afterwards
                  — the mesh just has fewer triangles. Use this when you want an asset to
                  ship at, say, 5,000 triangles instead of 50,000, regardless of camera distance.</li>
            </ul>
            <p className={s.para}>
              Both backends use Ogre's <Code>MeshLodGenerator</Code> (edge-collapse based,
              respects submesh boundaries). Decimation accepts three target modes: a raw
              reduction fraction (<Code>--reduction 0.5</Code> = drop half the tris), a target
              triangle count (<Code>--target-tris 5000</Code>), or a target vertex budget
              (<Code>--target-verts 2500</Code>). All three clamp to 95% max so we don't
              degenerate the mesh into a single triangle.
            </p>
            <p className={s.para}>
              The validator emits a "Tri budget" suggestion row when the active selection has
              more than ~10,000 triangles. It's a nudge, not a hard rule — desktop pipelines
              routinely ship higher poly counts. Set a stricter scan rule
              (<Code>max_vertex_count</Code>) if you need CI enforcement.
            </p>

            <h3 className={s.subsection}>Vertex cache (ACMR)</h3>
            <p className={s.para}>
              Modern GPUs cache the last ~32 transformed vertices (the "post-T&amp;L cache"). Triangles that
              reference recently-emitted vertices skip the per-vertex pipeline cost. <strong>ACMR</strong>
              (Average Cache Miss Ratio) measures how friendly a mesh's index order is to that cache —
              cache misses divided by triangle count.
            </p>
            <table className={s.table}>
              <thead><tr><th>ACMR range</th><th>Meaning</th></tr></thead>
              <tbody>
                <tr><td><Code>~0.5</Code></td><td>Theoretical optimum (each triangle reuses two cached verts).</td></tr>
                <tr><td><Code>0.5 – 1.0</Code></td><td>Well-ordered strips; no reorder needed.</td></tr>
                <tr><td><Code>1.0 – 2.0</Code></td><td>Typical for unoptimized exporters. Worth a reorder pass.</td></tr>
                <tr><td><Code>&gt; 2.0</Code></td><td>Random-ish topology. Reorder cuts vertex-shader load substantially.</td></tr>
              </tbody>
            </table>
            <p className={s.para}>
              <Code>qtmesh vertex-cache</Code> runs <a href="http://eelpi.gotdns.org/papers/fast_vert_cache_opt.html" className={s.link}>Tom Forsyth's linear-time algorithm</a>
              on every submesh. Without <Code>-o</Code> it's analyze-only and reports what the projected
              ACMR would be after a reorder. With <Code>-o &lt;out&gt;</Code> it rewrites the index buffer in
              place — but only when the new order strictly improves ACMR (never regresses). The Inspector's
              <Code>Optimize Vertex Cache</Code> button does the same in-memory rewrite without writing back
              to disk.
            </p>
            <p className={s.para}>
              The scan command's <Code>max_acmr</Code> rule flags assets above a configured ceiling.
              Scan loads each mesh through the editor's <Code>MeshImporterExporter</Code> and measures
              ACMR on Ogre's actual index buffer, so scan-side numbers match the in-editor validator
              one-for-one — set the same ceiling you'd accept in the editor.
            </p>
          </section>

          <CmdSection id="cmd-memory" name="memory" description="Estimate per-mesh GPU bytes plus per-texture VRAM. Returns a non-zero exit when an optional budget is exceeded — the CI gate for asset memory regressions."
            synopsis={`qtmesh memory <file> [--json] [--budget <size>] [--token <t>] [--no-cloud]`}
            options={[
              ['--json', 'Output the structured report as JSON'],
              ['--budget <size>', 'Memory ceiling. Accepts plain bytes or units: 50MB, 1.5GB, 2048KB. Exit 1 if exceeded.'],
              ['--token <t>', 'QtMesh Cloud ingest token. When set and --budget is omitted, the project\'s memory_budget_mb rule is fetched and used.'],
              ['--no-cloud', 'Opt out of the cloud-budget lookup (use the local default).'],
            ]}
            examples={[
              'qtmesh memory character.fbx',
              'qtmesh memory character.fbx --json',
              'qtmesh memory character.fbx --budget 50MB',
              'qtmesh memory character.fbx --budget 1.5GB --json',
              'QTMESH_TOKEN=… qtmesh memory character.fbx',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`Memory Report
=============

Meshes (1):
  character.mesh  v=12,584  i=58,002  524.3 KB
  TOTAL: 524.3 KB

Textures (3):
  diffuse.png       1024x1024   4Bpp  +mips   5.33 MB
  normal.png        1024x1024   4Bpp  +mips   5.33 MB
  metallicRough.png 512x512     4Bpp  +mips   1.33 MB
  TOTAL: 12.00 MB

Scene total: 12.51 MB
Budget:      50.00 MB`}</CodeBlock>
            <h3 className={s.subsection}>JSON shape</h3>
            <p className={s.para}>
              The JSON payload has <Code>meshes[]</Code>, <Code>textures[]</Code>, a <Code>totals</Code>
              object with <Code>meshBytes</Code>/<Code>textureBytes</Code>/<Code>totalBytes</Code>, and (when
              a budget is set) a <Code>budget</Code> object with <Code>bytes</Code> and <Code>overBudget</Code>.
              The same shape comes back from the MCP <Code>get_memory_usage</Code> tool under the
              <Code>memory</Code> key.
            </p>
          </CmdSection>

          <CmdSection id="cmd-analyze" name="analyze" description="Group every entity by the materials its submeshes use, count draw calls (one per SubEntity), and rank merge opportunities. Read-only — never modifies geometry."
            synopsis={`qtmesh analyze <file> [--json]`}
            options={[
              ['--json', 'Output the structured report as JSON'],
            ]}
            examples={[
              'qtmesh analyze level_environment.glb',
              'qtmesh analyze level_environment.glb --json',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`Draw Call Analysis
==================

Entities:       12
Submeshes:      18
Draw calls:     18
Unique mats:    5
After merges:   8 (saves 10)

Materials:
  Foliage         submeshes=4  entities=4
  Stone.Wall      submeshes=6  entities=5
  Stone.Floor     submeshes=4  entities=2
  Lantern         submeshes=2  entities=1
  Water           submeshes=2  entities=0

Merge suggestions (saves >0 draw calls):
  Stone.Wall  merge 5 entities → save 4 draw calls
    - wall_north
    - wall_south
    - wall_west_a
    - wall_west_b
    - wall_east
  Foliage     merge 4 entities → save 3 draw calls
    - tree_1
    - tree_2
    - bush_a
    - bush_b
  Stone.Floor merge 2 entities → save 1 draw calls
    - floor_main
    - floor_alcove`}</CodeBlock>
            <p className={s.para}>
              <strong>Reading the report:</strong> "Draw calls" is today's cost. "After merges" is the cost
              if every shared-material cluster were combined. The gap is what you'd save with batching.
              In the validator, the <Code>Draws:</Code> info row shows the same totals; in MCP the
              <Code>analyze_draw_calls</Code> tool returns the structured payload under the
              <Code>drawCalls</Code> key.
            </p>
          </CmdSection>

          <CmdSection id="cmd-vertex-cache" name="vertex-cache" description="Run Tom Forsyth's linear-time vertex-cache optimization on every submesh. Reports per-submesh ACMR before/after. Analyze-only by default; pass -o to rewrite the index buffer and export."
            synopsis={`qtmesh vertex-cache <file> [-o <output>] [--json]`}
            options={[
              ['-o <output>', 'Output file. When omitted, runs in analyze-only mode (index buffer is not modified).'],
              ['--json', 'Output the structured report as JSON'],
            ]}
            examples={[
              'qtmesh vertex-cache character.fbx                      # analyze-only',
              'qtmesh vertex-cache character.fbx -o optimized.fbx     # rewrite + export',
              'qtmesh vertex-cache character.fbx --json',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`File: ninja.mesh -> ninja_opt.mesh
Vertex Cache Analysis
=====================

  ninja.mesh [0]  tris=904  ACMR 0.971 → 0.871  (reordered)
  ninja.mesh [1]  tris=104  ACMR 0.587 → 0.587

Total triangles: 1,008
Weighted ACMR:   0.932 → 0.841  (9.7% improvement)
Submeshes rewritten: 1 of 2`}</CodeBlock>
            <p className={s.para}>
              <strong>Never regresses:</strong> the rewrite only happens when the new ACMR is strictly
              lower than the original. Submesh 1 above is already near-optimal (0.587), so it's not
              rewritten — the &quot;analyze-only&quot; column matches the &quot;after&quot; column for it.
            </p>
            <p className={s.para}>
              <strong>Inspector workflow:</strong> click <Code>Run Validation</Code>. If the cache row
              reports a meaningful improvement (≥1%), an <Code>Optimize Vertex Cache</Code> button appears.
              That button runs the reorder on Ogre's in-memory index buffer without writing to disk —
              export the scene to persist. The Inspector also shows the projected ACMR delta on the row
              even before you click.
            </p>
            <p className={s.para}>
              <strong>MCP:</strong> the <Code>optimize_vertex_cache</Code> tool takes a <Code>rewrite</Code>
              bool (default <Code>false</Code>) and returns the structured payload under the
              <Code>vertexCache</Code> key.
            </p>
          </CmdSection>

          <CmdSection id="cmd-decimate" name="decimate" description="Single-pass mesh decimation. Reduces the base mesh itself (unlike `lod` which builds a discrete LOD chain). Pick one of three target modes — by fraction, by triangle count, or by vertex budget. Always requires -o; never overwrites the input."
            synopsis={`qtmesh decimate <file> -o <output> --reduction <r> [--json]
qtmesh decimate <file> -o <output> --target-tris N [--json]
qtmesh decimate <file> -o <output> --target-verts N [--json]`}
            options={[
              ['-o <output>', 'Output file (required — decimation is destructive)'],
              ['--reduction <r>', 'Drop this fraction of triangles (0..0.95). 0.5 = 50% reduction.'],
              ['--target-tris N', 'Reduce to approximately N triangles total.'],
              ['--target-verts N', 'Reduce to approximately N vertices total.'],
              ['--json', 'Output the structured report as JSON'],
            ]}
            examples={[
              'qtmesh decimate character.fbx -o character_lo.fbx --reduction 0.5',
              'qtmesh decimate character.fbx -o character_mobile.glb2 --target-tris 5000',
              'qtmesh decimate character.fbx -o tiny.fbx --target-verts 1500 --json',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`File: ninja.mesh -> ninja_dec.mesh
Mesh Decimation
===============

Mesh: ninja.mesh
Reduction requested: 50.4%  (applied)

  [0] tris 904 → 456
  [1] tris 104 → 36

Total: 1,008 → 492 (51.2% effective reduction)`}</CodeBlock>
            <p className={s.para}>
              <strong>Caps at 95%</strong> — the slider / API never lets you drop below 5% of the
              original triangle count, which would degenerate the mesh into a single triangle in
              most cases. If you need to go further, run a second pass.
            </p>
            <p className={s.para}>
              <strong>UV seams / material boundaries:</strong> the underlying
              <Code>Ogre::MeshLodGenerator</Code> decimates each submesh independently and
              respects index-buffer boundaries, so a mesh split across multiple materials keeps its
              UV-island seams. True per-vertex weight locking (so you can mark cap edges as
              "never collapse this") is its own future slice — the current path uses uniform weights.
            </p>
            <p className={s.para}>
              <strong>MCP:</strong> the <Code>decimate_mesh</Code> tool takes one of
              <Code>reduction</Code> / <Code>target_tris</Code> / <Code>target_verts</Code> plus an
              optional <Code>dry_run</Code> bool (default <Code>false</Code>; when true, returns a
              projected report without mutating the scene). Response carries the structured
              payload under the <Code>decimation</Code> key.
            </p>
          </CmdSection>

          <CmdSection id="cmd-atlas" name="atlas" description={<>Pack N input textures into a single atlas image plus a JSON manifest of per-tile UV remaps. Shelf bin-pack with height-descending sort — deterministic; no rotation; tiles padded on every side to prevent MIP bleed. Pairs with the slice B <a href="#cmd-analyze" className={s.link}>draw-call analyzer</a>'s merge suggestions: collapse many small per-prop textures into a single binding and you cut the draw-call count proportionally.</>}
            synopsis={`qtmesh atlas --inputs a.png,b.png,c.png -o atlas.png
qtmesh atlas --inputs a.png,b.png --size 1024 --padding 4 -o atlas.png
qtmesh atlas --inputs a.png,b.png -o atlas.png --manifest atlas.json`}
            options={[
              ['--inputs <csv>', 'Comma-separated paths to input texture files (PNG/TGA/JPG/BMP). Bare positional args also work.'],
              ['-o <output>', 'Output atlas image path. Extension determines format (PNG/TGA/JPG).'],
              ['--size N', 'Convenience: sets both atlas width and height to a square. Default 2048.'],
              ['--width N --height N', 'Explicit per-axis atlas dimensions when not square.'],
              ['--padding N', 'Pixels of empty space on every side of a tile so MIPs don\'t bleed. Default 2.'],
              ['--manifest <json>', 'Optional path for the JSON manifest of per-tile UV remaps.'],
            ]}
            examples={[
              'qtmesh atlas --inputs props/wood.png,props/metal.png,props/stone.png -o props_atlas.png --manifest props_atlas.json',
              'qtmesh atlas --inputs character_diffuse.png,character_normal.png --size 4096 --padding 8 -o character_atlas.png',
            ]}
          >
            <h3 className={s.subsection}>Manifest Schema</h3>
            <CodeBlock lang="json">{`{
  "width": 2048,
  "height": 2048,
  "padding": 2,
  "tiles": [
    {
      "source": "props/wood.png",
      "x": 2, "y": 2, "w": 512, "h": 512,
      "u0": 0.000976, "v0": 0.000976,
      "u1": 0.250976, "v1": 0.250976
    }
  ]
}`}</CodeBlock>
            <p className={s.para}>
              Each tile carries both pixel coordinates and pre-computed UV ranges. Downstream
              tooling (asset-pipeline scripts, custom build steps, future "Apply Atlas" Inspector
              action) can ingest the manifest directly to rewrite mesh UV0 onto the atlas.
            </p>
            <h3 className={s.subsection}>Algorithm</h3>
            <p className={s.para}>
              Inputs are sorted by padded-height descending and placed left-to-right on the
              current shelf. When the next tile doesn't fit width-wise, a new shelf is opened
              below at the existing shelf's bottom edge. Returns an error if any single tile
              exceeds the atlas after padding, or if the requested atlas can't fit the full
              input set.
            </p>
            <p className={s.para}>
              <strong>MCP:</strong> the <Code>pack_atlas</Code> tool accepts the same shape —
              <Code>inputs</Code> can be either a JSON array of strings or a comma-separated
              single string. Response carries <Code>tiles_packed</Code>, <Code>atlas_width</Code>,
              <Code>atlas_height</Code>, <Code>used_width</Code>, <Code>used_height</Code>, and
              the <Code>output</Code>/<Code>manifest</Code> paths.
            </p>
          </CmdSection>

          <CmdSection id="cmd-optimize" name="optimize" description={<>Batch-optimize a single mesh asset end-to-end. Sequences the slice C / C4 / D optimizations on the same loaded Ogre scene with no intermediate file I/O: <a href="#cmd-vertex-cache" className={s.link}>vertex-cache reorder</a> → optional <a href="#cmd-decimate" className={s.link}>decimation</a> → animation keyframe simplify. Defaults to <Code>--vertex-cache --simplify-anim</Code> when no flags are given. Add <Code>--reduction</Code> / <Code>--target-tris</Code> / <Code>--target-verts</Code> to also decimate. Each stage emits an applied/summary report (text or <Code>--json</Code>).</>}
            synopsis={`qtmesh optimize <file> -o <output>
qtmesh optimize <file> -o <output> --all
qtmesh optimize <file> -o <output> --reduction 0.5
qtmesh optimize <file> -o <output> --target-tris 5000 --simplify-rotation-deg-tol 1.0
qtmesh optimize <file> -o <output> --vertex-cache  # just vertex-cache, skip anim simplify`}
            options={[
              ['-o <output>', 'Output file (required — optimize is potentially destructive when decimation is enabled)'],
              ['--vertex-cache', 'Forsyth reorder every triangulated submesh (default on)'],
              ['--simplify-anim', 'Strip redundant animation keyframes (default on)'],
              ['--all', 'Convenience: --vertex-cache + --simplify-anim (does not enable decimation)'],
              ['--reduction <r>', 'Drop this fraction of triangles (0..0.95). 0.5 = 50% reduction'],
              ['--target-tris N', 'Reduce to approximately N triangles total'],
              ['--target-verts N', 'Reduce to approximately N vertices total'],
              ['--simplify-translation-tol T', 'Anim simplify translation tolerance in world units (default 0.0001 = Conservative — simplify is destructive, so the safe choice)'],
              ['--simplify-rotation-deg-tol D', 'Anim simplify rotation tolerance in degrees (default 0.05 = Conservative)'],
              ['--simplify-scale-tol S', 'Anim simplify scale tolerance (default 0.0001 = Conservative)'],
              ['--json', 'Emit the per-stage report as JSON instead of text'],
            ]}
            examples={[
              'qtmesh optimize character.fbx -o character_opt.fbx',
              'qtmesh optimize character.fbx --reduction 0.5 -o character_lo.fbx',
              'qtmesh optimize character.fbx --target-tris 5000 --simplify-rotation-deg-tol 1.0 -o lo.fbx',
              'qtmesh optimize prop.glb --vertex-cache -o prop_reordered.glb',
            ]}
          >
            <h3 className={s.subsection}>Example Output</h3>
            <CodeBlock>{`File: Rumba Dancing.fbx -> rumba_opt.fbx
Mesh Optimization
=================

  [OK] vertex-cache: ACMR 0.822 -> 0.648 across 10220 triangles, 9 submeshes rewritten
  [OK] simplify-anim: removed 1156 / 2750 keyframes (42.0%)

  6310 KB -> 1405 KB  (4904 KB saved, 77.7%)`}</CodeBlock>
            <p className={s.para}>
              <strong>Same code paths everywhere.</strong> The vertex-cache stage runs
              <Code>VertexCacheOptimizer::analyzeEntity</Code> (the slice C optimizer), the
              decimate stage runs <Code>MeshDecimator::decimateEntity</Code> (the slice D
              one-pass reducer), and the simplify-anim stage runs
              <Code>AnimationMerger::simplifyAnimation</Code> (the same analyzer behind
              <Code> qtmesh anim --simplify</Code> and the Inspector "Simplify" button). So
              `optimize` reports match the per-stage CLI commands one-for-one.
            </p>
            <p className={s.para}>
              <strong>MCP:</strong> the <Code>optimize_mesh</Code> tool takes
              <Code>file</Code> + <Code>output</Code> plus the same flag set as keys
              (<Code>vertex_cache</Code>, <Code>simplify_anim</Code>, <Code>reduction</Code>,
              <Code>target_tris</Code>, <Code>target_verts</Code>, <Code>simplify_*_tol</Code>).
              Response carries per-stage <Code>applied</Code>/<Code>summary</Code>/<Code>details</Code>
              plus <Code>inputBytes</Code>/<Code>outputBytes</Code>/<Code>bytesDelta</Code>.
            </p>
          </CmdSection>

          <CmdSection id="cmd-uv" name="uv" description={<>Automatic UV unwrap via <a href="https://github.com/jpcy/xatlas" className={s.link}>xatlas</a> — the MIT library Blender and Godot use under the hood. Segments the mesh into <em>charts</em> (groups of connected faces with low distortion), parameterizes each chart to 2D, and packs them into a single rectangular atlas. Useful for lightmap baking (write to UV1), texture baking on photogrammetry / sculpted / procedural meshes that ship without UVs, repairing degenerate authored UVs, and atlasing assets so they can share a single texture binding (combine with <a href="#cmd-atlas" className={s.link}>qtmesh atlas</a>). Skin weights and other vertex attributes survive the seam-split remap.</>}
            synopsis={`qtmesh uv <file> --info [--json]
qtmesh uv <file> --unwrap -o <out>
qtmesh uv <file> --unwrap --channel 1 -o lightmap.glb
qtmesh uv <file> --unwrap --resolution 2048 --padding 4 -o out.glb`}
            options={[
              ['--info', 'Report current UV channels and UV0 bounding-box coverage per submesh. Does not modify the mesh. Pair with --json for machine-readable output.'],
              ['--unwrap', 'Generate a non-overlapping atlas of UVs and write them into the target channel.'],
              ['-o <output>', 'Output mesh file (required with --unwrap). Extension determines format: .glb / .gltf / .fbx / .mesh / .obj / .dae / .stl / .ply.'],
              ['--resolution N', 'Atlas size hint (default 1024). xatlas estimates texels-per-world-unit to roughly match this.'],
              ['--padding P', 'Texels of padding around each chart (default 4 — safe up to MIP level 2). Prevents MIP-mapped sampling from bleeding across chart boundaries.'],
              ['--channel C', 'UV channel index to write into (default 0). Channel 0 overwrites the primary UV; 1+ keeps UV0 and writes into a higher channel (lightmap workflow).'],
              ['--no-backup', 'Disable preserving the previous UV channel on UV{C+1} when overwriting. Default behavior preserves it so artistic UVs aren\'t lost.'],
              ['--json', 'Emit the report as JSON instead of text.'],
            ]}
            examples={[
              'qtmesh uv character.fbx --info --json',
              'qtmesh uv photogrammetry_scan.obj --unwrap -o scan_with_uvs.glb',
              'qtmesh uv level_geometry.fbx --unwrap --channel 1 --resolution 2048 -o level_lightmap.glb',
              'qtmesh uv prop.glb --unwrap --resolution 512 --padding 2 -o prop_atlased.glb',
            ]}
          >
            <h3 className={s.subsection}>What is this useful for?</h3>
            <p className={s.para}>
              UV unwrap solves a specific problem: <strong>you have a 3D model whose
              UVs are missing, broken, or unsuitable for a particular workflow, and
              you need clean non-overlapping UVs without doing the unwrap by hand
              in Blender/Maya.</strong> Common reasons to reach for it:
            </p>
            <ul className={s.list}>
              <li>
                <strong>Lightmap baking (the most common reason).</strong> Real-time
                engines (Unreal, Unity, Godot) bake static lighting into a texture
                per object. That bake <em>requires</em> every triangle to occupy
                its own unique patch of UV space — no overlaps, no mirroring —
                because two triangles sharing a UV pixel would receive conflicting
                light values. Almost no artist-authored UV0 satisfies this.
                Workflow: <Code>--unwrap --channel 1</Code> → engine uses UV0 for
                diffuse, UV1 for the lightmap.
              </li>
              <li>
                <strong>Texture / AO / curvature baking.</strong> Bakers write pixels
                into UV space — they need non-overlapping UVs to know which
                triangle owns each pixel. Mirrored or overlapping authored UVs
                (common for symmetric characters) make AO bake as garbage.
              </li>
              <li>
                <strong>Imported assets with no UVs at all.</strong> Photogrammetry
                meshes, ZBrush sculpts, raw CAD / STL / PLY files, procedurally
                generated geometry. None of these have usable UVs out of the box;
                unwrap gives you a starting point so you can texture them at all.
              </li>
              <li>
                <strong>Broken UVs.</strong> FBX / glTF that came through a sketchy
                pipeline (3D scanners, game-rip tools, old DAE exports) where UVs
                are degenerate, all collapsed into one corner, or completely
                scrambled.
              </li>
              <li>
                <strong>Texture atlasing.</strong> Combining many small textures into
                one big atlas for draw-call reduction (paired with{' '}
                <a href="#cmd-atlas" className={s.link}>qtmesh atlas</a>). Each tile
                needs its own UV region; unwrapping first guarantees that.
              </li>
              <li>
                <strong>Mesh decimation cleanup.</strong> After heavy decimation, the
                surviving UV islands may have ugly distortion or seams that no
                longer make sense. Re-unwrapping gives clean topology-aware UVs for
                the decimated result.
              </li>
              <li>
                <strong>Quick texturing for prototypes.</strong> You just authored a
                primitive or CSG shape in the editor and want to slap a texture on
                it — auto-unwrap → texture → done.
              </li>
            </ul>
            <p className={s.para}>
              <strong>When NOT to use it.</strong> If an artist hand-painted onto
              specific UV islands of UV0 (most game character diffuse maps),
              auto-unwrap will discard their layout — the existing texture will
              sample from random atlas tiles. For that case either unwrap into
              UV1 (<Code>--channel 1</Code>, default <Code>--no-backup</Code> is off
              so UV0 is preserved untouched), or bake a new texture against the
              new UVs, or do the unwrap by hand in Blender where you can mark
              seams manually.
            </p>

            <h3 className={s.subsection}>Example output</h3>
            <CodeBlock>{`UV Unwrap
=========
Mesh:          Rumba Dancing
Submeshes:     11
Vertices:      5828 → 8423  (+2595 from seam splits)
Triangles:     10220
Atlas:         1246x1239
Charts:        329
Utilization:   75.7%
Wrote: rumba_unwrapped.glb`}</CodeBlock>
            <p className={s.para}>
              <strong>Vertex count grows.</strong> xatlas splits vertices along chart
              seams so each chart has its own copy of the seam edge with its own
              UV — this is unavoidable (without splits the chart boundaries would
              share UVs, defeating the point). The seam-split factor is typically
              1.3x–1.5x for character meshes.
            </p>
            <p className={s.para}>
              <strong>Utilization</strong> reports the fraction of the atlas image
              that's covered by actual UV-space charts (the rest is padding +
              dead area between charts). 70–80% is normal; xatlas's packer is
              deterministic shelf bin-pack, not optimal.
            </p>

            <h3 className={s.subsection}>GUI workflow</h3>
            <p className={s.para}>
              Select a mesh entity, switch to <strong>Material Mode</strong>, click{' '}
              <Code>Auto UV Unwrap…</Code> in the Mode Tools section, pick output
              path + resolution / padding / channel, then click{' '}
              <Code>Export Unwrapped</Code>. The result is written to a new file
              — your on-screen mesh is left untouched. Open the exported file
              to view the result alongside the original.
            </p>
            <p className={s.para}>
              Why "export to file" instead of "apply in place"? Live skinned
              meshes can't survive in-place vertex-data mutation while rendering:
              the active SkeletonInstance caches the hardware blend buffer and
              picks up stale state on the first frame after the swap. The dialog
              snapshots the entity, runs the unwrap, exports, then restores the
              live entity bit-identical to its pre-unwrap state.
            </p>

            <h3 className={s.subsection}>MCP</h3>
            <p className={s.para}>
              The <Code>auto_uv_unwrap</Code> tool takes <Code>file</Code> +{' '}
              <Code>output</Code> plus <Code>resolution</Code> / <Code>padding</Code>{' '}
              / <Code>channel</Code> / <Code>preserve_original</Code>. Returns a
              structured <Code>unwrap</Code> object with atlas size, chart count,
              vert count before/after, and utilization.
            </p>
          </CmdSection>

          {/* ─── Scan Reference ─── */}

          <section className={s.section} id="scan-config">
            <h2 className={s.sectionTitle}>Configuration (qtmesh.yml)</h2>
            <p className={s.para}>
              The scan command automatically loads <Code>qtmesh.yml</Code>, <Code>qtmesh.yaml</Code>, or <Code>qtmesh.json</Code> from the current directory. Override with <Code>--config &lt;path&gt;</Code>.
            </p>

            <h3 className={s.subsection}>Full Schema</h3>
            <CodeBlock lang="yaml">{`version: 1

# Optional built-in platform profile (see Platform Profiles below)
# profile: modern-console

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

  # Vertex-cache friendliness (ACMR — see Performance Concepts)
  max_acmr: 1.0                 # Warn when ACMR exceeds this; 0 = disabled

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

  # Animation simplification (fixable via --fix; same analyzer as
  # qtmesh anim --simplify and the in-app Simplify button)
  redundant_keyframes_pct: 40
  redundant_keyframes_translation_tol: 0.001
  redundant_keyframes_rotation_deg_tol: 0.5
  redundant_keyframes_scale_tol: 0.001

  # Quality rules (slice C4) — all opt-in
  max_texture_resolution: 2048           # px on longest edge
  require_uv_channels: 1                 # 0 = off, 1 = any UV, 2 = lightmap
  detect_zero_weight_bones: true         # info: Mixamo-style unused bones
  detect_overlapping_uvs_pct: 5.0        # warn at >= 5% overlapping UV0 AABBs
  detect_non_manifold_edges_pct: 1.0     # warn at >= 1% non-manifold edges

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

          <section className={s.section} id="scan-profiles">
            <h2 className={s.sectionTitle}>Platform Profiles</h2>
            <p className={s.para}>
              Platform profiles are bundled JSON presets that apply conservative triangle, material,
              texture, bone, and animation budgets tuned for a target class of hardware or engine
              pipeline. They validate <strong>source</strong> assets (FBX, glTF, OBJ, VRM) before
              you import into Unreal, Unity, Godot, or a retro toolchain — they do <em>not</em> export
              cooked platform binaries, ROM-ready geometry, or proprietary SDK formats.
            </p>
            <p className={s.para}>
              Precedence: scanner defaults → platform profile → project <Code>qtmesh.yml</Code> →
              CLI flags. Set a profile in YAML with <Code>profile: &lt;id&gt;</Code>, or pass
              <Code>--target &lt;id&gt;</Code> / <Code>--profile &lt;id&gt;</Code> on the command line
              (CLI wins). List bundled ids with <Code>qtmesh scan --list-profiles</Code>.
              Custom profiles can be any <Code>.json</Code> file on disk — pass the path to
              <Code>--profile</Code>.
            </p>

            <h3 className={s.subsection}>In the editor (Validation mode)</h3>
            <p className={s.para}>
              Switch to <strong>Validation</strong> mode in the workspace toolbar, then expand
              <strong> Asset Folder Scan</strong> in the Inspector. Pick a platform profile from the
              dropdown (default: <strong>Modern Console</strong>), choose an assets folder with
              <strong> Browse…</strong>, and run <strong>Scan Folder</strong>. Results appear inline
              with the same rule severities as CLI scan. Example-only profiles used by unit tests
              (<Code>example-*</Code>) are hidden in the GUI but still available from the CLI.
            </p>

            <h3 className={s.subsection}>Bundled profiles</h3>
            <p className={s.para}>
              Profiles ship in the <Code>profiles/</Code> directory next to the app binary. Key
              budget columns below are starting points — tune per title and combine with
              <Code>scopes</Code> in <Code>qtmesh.yml</Code> for path-specific overrides.
            </p>
            <table className={s.table}>
              <thead>
                <tr>
                  <th>Id</th>
                  <th>Name</th>
                  <th>Category</th>
                  <th>Max tris</th>
                  <th>Max texture</th>
                  <th>Materials</th>
                  <th>Draw calls</th>
                  <th>Bones</th>
                </tr>
              </thead>
              <tbody>
                <tr><td><Code>modern-console</Code></td><td>Modern Console</td><td>Modern</td><td>250k</td><td>4096 px</td><td>32</td><td>128</td><td>256</td></tr>
                <tr><td><Code>switch-like</Code></td><td>Switch-class</td><td>Modern</td><td>80k</td><td>2048 px</td><td>16</td><td>32</td><td>128</td></tr>
                <tr><td><Code>steamdeck</Code></td><td>Steam Deck</td><td>Modern</td><td>150k</td><td>4096 px</td><td>24</td><td>64</td><td>200</td></tr>
                <tr><td><Code>mobile-low</Code></td><td>Mobile Low-end</td><td>Modern</td><td>25k</td><td>1024 px</td><td>8</td><td>16</td><td>64</td></tr>
                <tr><td><Code>webgl</Code></td><td>WebGL / WebGPU</td><td>Modern</td><td>40k</td><td>2048 px</td><td>12</td><td>24</td><td>96</td></tr>
                <tr><td><Code>vr</Code></td><td>VR</td><td>Modern</td><td>70k</td><td>2048 px</td><td>12</td><td>20</td><td>96</td></tr>
                <tr><td><Code>ps1</Code></td><td>PlayStation 1</td><td>Retro</td><td>5k</td><td>256 px</td><td>4</td><td>8</td><td>16</td></tr>
                <tr><td><Code>n64</Code></td><td>Nintendo 64</td><td>Retro</td><td>12k</td><td>256 px</td><td>8</td><td>12</td><td>32</td></tr>
                <tr><td><Code>nds</Code></td><td>Nintendo DS</td><td>Retro</td><td>4k</td><td>256 px</td><td>4</td><td>8</td><td>16</td></tr>
                <tr><td><Code>dreamcast</Code></td><td>Sega Dreamcast</td><td>Retro</td><td>15k</td><td>512 px</td><td>12</td><td>16</td><td>64</td></tr>
              </tbody>
            </table>
            <p className={s.para}>
              Modern profiles also enable texture inspection (<Code>inspect_textures</Code> metadata),
              power-of-two texture warnings, allowed texture format lists, ACMR ceilings, redundant
              keyframe detection, and zero-weight bone reporting where applicable. Retro profiles
              focus on tight geometry and texture budgets suited to demake and homebrew workflows.
            </p>

            <h3 className={s.subsection}>Examples</h3>
            <CodeBlock>{`# List bundled profile ids
qtmesh scan --list-profiles

# Scan with a handheld-class budget
qtmesh scan ./assets --target switch-like --fail-on warning

# Pin profile in project config (CLI --target/--profile overrides this)
# qtmesh.yml:
profile: mobile-low

# Custom profile JSON on disk
qtmesh scan ./assets --profile ./ci/my-studio-profile.json`}</CodeBlock>
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

            <h3 className={s.subsection}>Performance Rules</h3>
            <RuleCard name="max_acmr" type="number" severity="warning"
              description={<>Maximum acceptable <strong>Average Cache Miss Ratio</strong>. Flags meshes whose
                            index buffer reorders poorly for the GPU vertex cache. See the <a href="#perf-overview" className={s.link}>Performance Concepts</a> page
                            for the formula. Scan loads each mesh through the editor's
                            <Code>MeshImporterExporter</Code> and measures ACMR on Ogre's actual index buffer,
                            so scan numbers match the in-editor validator's one-for-one — set the same ceiling
                            you'd accept in the editor (around <Code>1.0</Code> is typical). Fix with
                            <Code>qtmesh vertex-cache &lt;in&gt; -o &lt;out&gt;</Code>.</>}
              example={`max_acmr: 1.0    # 0 = disabled`} />

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

            <h3 className={s.subsection}>Animation Simplification</h3>
            <RuleCard name="redundant_keyframes_pct" type="number" severity="warning"
              description={<>Warn when a tolerance-based simplifier could remove at least
                this percentage of animation keyframes. Uses the same analyzer as
                <Code>qtmesh anim --simplify</Code> and the in-app "Simplify" button — so
                the percentage the scan flags is exactly the percentage the auto-fix would
                remove. The rule is <strong>fixable</strong>: <Code>qtmesh scan ... --fix</Code> re-exports
                the asset (FBX / glTF / DAE / OBJ / PLY / STL / .mesh) with the simplified
                animation, only replacing the original if the rewrite isn't materially larger.
                Set the three tolerance keys to tune what counts as redundant.</>}
              example={`redundant_keyframes_pct: 40
redundant_keyframes_translation_tol: 0.001    # ~1mm on meter-scale rigs
redundant_keyframes_rotation_deg_tol: 0.5
redundant_keyframes_scale_tol: 0.001`} />

            <h3 className={s.subsection}>Quality Rules (Slice C4)</h3>
            <p className={s.para}>
              All quality rules are driven by the same Ogre scene walk that powers
              the in-app <Code>MeshInfoOverlay</Code>, so a value reported by the scan
              matches what the editor would show for the same asset. All are <strong>opt-in</strong>
              (default off) — none of these fire on a fresh <Code>qtmesh.yml</Code> until you
              explicitly enable them.
            </p>

            <RuleCard name="max_texture_resolution" type="number" severity="warning"
              description={<>Cap on the largest single-axis pixel dimension across every bound
                texture. Walks each <Code>TextureUnitState</Code>'s resolved <Code>Ogre::Texture</Code>
                and checks <Code>max(width, height)</Code> against the threshold. <Code>0</Code> = disabled.
                Typical: <Code>2048</Code> for mobile / web, <Code>4096</Code> for desktop. Pack with
                <Code>qtmesh pack-textures</Code> to consolidate ORM-style maps and reduce VRAM cost.</>}
              example={`max_texture_resolution: 2048    # px on longest edge`} />

            <RuleCard name="require_uv_channels" type="number" severity="warning"
              description={<>Minimum number of UV sets every submesh must carry. Counts
                <Code>VES_TEXTURE_COORDINATES</Code> elements in the vertex declaration of the
                <em> least</em>-equipped submesh, so a single submesh missing UV1 trips the rule.
                <Code>0</Code> = disabled. <Code>1</Code> requires base UV0 (anything texture-mapped).
                <Code>2</Code> is the lightmap-workflow setting — every submesh needs a non-overlapping
                second UV channel for baked lighting.</>}
              example={`require_uv_channels: 2    # 0 = off, 1 = any UV, 2 = lightmap`} />

            <RuleCard name="detect_zero_weight_bones" type="boolean" severity="info"
              description={<>Reports skeleton bones that exist in the hierarchy but have no
                <Code>VertexBoneAssignment</Code> on any submesh. Common Mixamo bloat — armatures
                ship with 70 bones but only ~30 actually influence vertices. Info-level by design,
                because stripping unused bones requires a DCC round-trip rather than an auto-fix.</>}
              example={`detect_zero_weight_bones: true`} />

            <RuleCard name="detect_overlapping_uvs_pct" type="number" severity="warning"
              description={<>Warn when at least this percentage of triangles have a UV0
                axis-aligned bounding box overlapping another triangle's. <Code>0</Code> = disabled.
                This is an <em>upper bound</em> on true UV overlap — if two AABBs don't intersect, the
                triangles can't overlap, but intersecting AABBs may still be disjoint when their UVs
                form a tight diagonal. Cheap O(n log n) sweep that catches the common cases:
                Mixamo body / clothing islands stacked at the same UV range, mirrored geometry on
                a shared lightmap unwrap, etc. Bake lightmaps will smear on these areas.</>}
              example={`detect_overlapping_uvs_pct: 5.0    # warn at 5% or higher`} />

            <RuleCard name="detect_non_manifold_edges_pct" type="number" severity="warning"
              description={<>Warn when at least this percentage of edges are <em>non-manifold</em> —
                shared by something other than exactly two triangulated faces. <Code>0</Code> = disabled.
                Boolean operations, fluid sims, 3D printing slicers and many shading pipelines expect
                manifold input; non-manifold edges break booleans, leak water sims and confuse slicer
                infill. Boundary edges (one face) count here too, which catches open-bottom clothing
                meshes from Mixamo and similar exports. Fix by welding duplicate verts and capping
                open boundaries in your DCC.</>}
              example={`detect_non_manifold_edges_pct: 1.0    # 1% manifold-violation tolerance`} />

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
                <tr>
                  <td><Code>redundant_keyframes_pct</Code></td>
                  <td>
                    Re-exports the asset (FBX / glTF / glb / DAE / OBJ / PLY / STL / .mesh) with redundant animation
                    keyframes removed via <Code>AnimationMerger::simplifyAnimation</Code> — the same routine as
                    <Code> qtmesh anim --simplify</Code> and the in-app "Simplify" button. The fix is skipped when the
                    rewrite would be materially larger than the original.
                  </td>
                </tr>
              </tbody>
            </table>
            <p className={s.para}>
              Additional fix capabilities (mesh optimization, format conversion) are planned. Use <Code>qtmesh fix</Code> for per-file mesh optimization.
            </p>
          </section>

          {/* ─── Integration ─── */}

          <section className={s.section} id="qtmesh-cloud">
            <h2 className={s.sectionTitle}>QtMesh Cloud Badges</h2>
            <p className={s.para}>
              Use <a href="https://qtmesh.dev" target="_blank" rel="noopener" style={{color: 'var(--accent-1)'}}>QtMesh Cloud</a> to
              store scan history and serve live SVG badges from real CI results.
            </p>

            <h3 className={s.subsection}>1. Register project and token</h3>
            <ol className={s.para} style={{ marginTop: '0.6rem', paddingLeft: '1.25rem' }}>
              <li>Sign in at <Code>https://qtmesh.dev</Code> and create your project.</li>
              <li>Choose a project slug (for example <Code>my-game-assets</Code>).</li>
              <li>Create a project token and save it in GitHub Secrets as <Code>QTMESH_CLOUD_TOKEN</Code>.</li>
            </ol>

            <h3 className={s.subsection}>2. Upload scan report from CI</h3>
            <CodeBlock lang="yaml">{`- name: Scan assets
  run: |
    docker run --rm \\
      -v "\${{ github.workspace }}:/workspace" \\
      -w /workspace \\
      ghcr.io/fernandotonon/qtmesh:latest \\
      scan --config /workspace/qtmesh.yml --json > qtmesh-scan-report.json

- name: Upload scan to QtMesh Cloud
  env:
    QTMESH_CLOUD_TOKEN: \${{ secrets.QTMESH_CLOUD_TOKEN }}
    QTMESH_CLOUD_API_URL: https://api.qtmesh.dev
  run: |
    jq --arg branch "\${GITHUB_HEAD_REF:-$GITHUB_REF_NAME}" \\
       --arg sha "\${GITHUB_SHA}" \\
       --arg runId "\${GITHUB_RUN_ID}" \\
       '. + {meta: {branch: $branch, commitSha: $sha, runId: $runId}}' \\
       qtmesh-scan-report.json > qtmesh-scan-upload.json

    curl --fail --silent --show-error \\
      -X POST "\${QTMESH_CLOUD_API_URL}/v1/ingest/scan" \\
      -H "Authorization: Bearer \${QTMESH_CLOUD_TOKEN}" \\
      -H "Content-Type: application/json" \\
      --data-binary @qtmesh-scan-upload.json`}</CodeBlock>

            <h3 className={s.subsection}>3. Use live badge URLs</h3>
            <CodeBlock lang="md">{`[![qtmesh status](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-warnings.svg)](https://qtmesh.dev)`}</CodeBlock>

            <p className={s.para}>
              Badge values update after each successful upload to <Code>/v1/ingest/scan</Code>.
            </p>
          </section>

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
  validate model.fbx

# Turntable preview PNG
docker run --rm -v $(pwd):/workspace ghcr.io/fernandotonon/qtmesh \\
  turntable model.fbx -o preview.png --frames 12 --size 512`}</CodeBlock>
          </section>

          <section className={s.section} id="github-actions">
            <h2 className={s.sectionTitle}>GitHub Actions</h2>
            <p className={s.para}>
              The <Code>qtmesh</Code> action is available on the <a href="https://github.com/marketplace/actions/qtmesheditor" className={s.link}>GitHub Actions Marketplace</a>.
              The snippets below substitute the latest GitHub release tag via the GitHub API (cached in the browser). Offline or rate-limited fallbacks use the semver pinned in <Code>CMakeLists.txt</Code> and kept in sync by <Code>scripts/sync-doc-versions-from-cmake.sh</Code>.
              Current resolved ref: <Code>{qtmeshActionRef}</Code>.
            </p>

            <h3 className={s.subsection}>Onboarding Step 3 template</h3>
            <CodeBlock lang="yaml">{`name: QtMesh Scan

on:
  push:
    branches: [ "master" ]

jobs:
  scan-assets-qtmesh:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Run QtMesh scan
        uses: ${qtmeshActionRef}
        with:
          command: scan
          image-tag: '${qtmeshImageTag}'
        env:
          QTMESH_CLOUD_TOKEN: \${{ secrets.QTMESH_CLOUD_TOKEN }}`}</CodeBlock>
            <p className={s.para}>
              Set <Code>QTMESH_CLOUD_TOKEN</Code> in repository secrets to upload scan results to QtMesh Cloud.
              Add <Code>qtmesh-strict-upload: true</Code> if upload failures should fail the job.
            </p>

            <h3 className={s.subsection}>Convert and validate</h3>
            <CodeBlock lang="yaml">{`- uses: ${qtmeshActionRef}
  with:
    command: validate
    input-file: ./models/character.fbx
    image-tag: '${qtmeshImageTag}'

- uses: ${qtmeshActionRef}
  with:
    command: convert
    input-file: ./models/character.fbx
    output-file: ./output/character.glb2
    image-tag: '${qtmeshImageTag}'

- uses: ${qtmeshActionRef}
  with:
    command: anim
    input-file: ./animations/dance.fbx
    output-file: ./output/dance_optimized.fbx
    options: --resample 30
    image-tag: '${qtmeshImageTag}'`}</CodeBlock>

            <h3 className={s.subsection}>Get mesh info as JSON</h3>
            <CodeBlock lang="yaml">{`- uses: ${qtmeshActionRef}
  id: info
  with:
    command: info
    input-file: ./models/character.fbx
    options: --json
    image-tag: '${qtmeshImageTag}'

- run: echo "\${{ steps.info.outputs.result }}"`}</CodeBlock>

            <h3 className={s.subsection}>Self-hosted scan badges (legacy Shields endpoint JSON)</h3>
            <CodeBlock lang="yaml">{`- uses: ${qtmeshActionRef}
  id: scan
  with:
    command: scan
    input-file: .
    options: --config /workspace/qtmesh.yml --json
    image-tag: '${qtmeshImageTag}'
    generate-badges: true
    badge-output-dir: badges
    badge-label-prefix: qtmesh
    badge-base-url: https://<USER>.github.io/<REPO>/badges

- name: Publish badges to gh-pages/badges
  uses: peaceiris/actions-gh-pages@v4
  with:
    github_token: \${{ secrets.GITHUB_TOKEN }}
    publish_dir: ./badges
    destination_dir: badges
    keep_files: true

- run: |
    echo "Status badge URL:"
    echo "\${{ steps.scan.outputs.badge-status-url }}"`}</CodeBlock>

            <h3 className={s.subsection}>README badge snippet</h3>
            <CodeBlock lang="md">{`[![qtmesh status](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-status.json)](https://github.com/<USER>/<REPO>/actions)
[![qtmesh errors](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-errors.json)](https://github.com/<USER>/<REPO>/actions)
[![qtmesh warnings](https://img.shields.io/endpoint?url=https%3A%2F%2F<USER>.github.io%2F<REPO>%2Fbadges%2Fqtmesh-warnings.json)](https://github.com/<USER>/<REPO>/actions)`}</CodeBlock>

            <h3 className={s.subsection}>Direct Docker usage</h3>
            <CodeBlock lang="yaml">{`- name: Scan assets
  run: |
    docker run --rm \\
      -v \${{ github.workspace }}:/workspace \\
      ghcr.io/fernandotonon/qtmesh:latest \\
      scan /workspace/assets \\
      --config /workspace/qtmesh.yml \\
      --fail-on error`}</CodeBlock>
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
  image: ghcr.io/fernandotonon/qtmesh:latest
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
