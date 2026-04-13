export const links = {
  github: 'https://github.com/fernandotonon/QtMeshEditor',
  docs: './docs.html',
  releases: 'https://github.com/fernandotonon/QtMeshEditor/releases/latest',
  allReleases: 'https://github.com/fernandotonon/QtMeshEditor/releases',
  issues: 'https://github.com/fernandotonon/QtMeshEditor/issues',
  forum: 'https://forums.ogre3d.org/viewtopic.php?t=76016',
  license: 'https://opensource.org/license/mit',
  actions: 'https://github.com/fernandotonon/qtmesh',
  marketplace: 'https://github.com/marketplace/actions/qtmesheditor'
};

export const media = {
  mergeDemo: {
    src: 'https://github.com/user-attachments/assets/441f90c5-1968-4838-8001-4ca24856a501',
    alt: 'QtMeshEditor animation merge workflow with imported FBX assets'
  },
  pipelineCiCd: {
    src: 'https://github.com/user-attachments/assets/1c7a7965-4bec-4e6d-8197-78d4435c8d46',
    alt: 'QtMeshEditor CI/CD pipeline overview'
  },
  skeletonPreview: {
    src: 'https://github.com/user-attachments/assets/289403ac-8952-488c-bc65-0a768ab278e1',
    alt: 'Bone weight and skeleton validation preview in QtMeshEditor'
  },
  mcpPreview: {
    src: 'https://github.com/user-attachments/assets/ed3b7e9d-22ba-4e6e-a06c-868570db7a07',
    alt: 'MCP tool integration controlling QtMeshEditor'
  },
  aiMaterials: {
    src: 'https://github.com/user-attachments/assets/c58978d7-7564-41f2-8c95-527ddf7ae78e',
    alt: 'AI-assisted material editor in QtMeshEditor'
  }
};

export const hero = {
  title: 'Automate your 3D asset pipeline.',
  subtitle:
    'QtMeshEditor gives indie teams and studios one technical workflow to fix, convert, merge, and automate 3D assets with GUI + CLI + CI/CD support, with repo-wide scanning and validation evolving quickly.',
  ctaPrimary: 'Download Latest',
  ctaSecondary: 'View on GitHub',
  ctaDocs: 'Documentation'
};

export const proofPoints = ['Free & open source', 'Windows / macOS / Linux', 'GUI + CLI', 'Docker + GitHub Actions'];

export const pipelineFlow = [
  {
    title: 'Scan',
    body: 'Pipeline scanning is in active development, focused on repo-wide checks for broken files and policy issues.'
  },
  {
    title: 'Validate',
    body: 'Evolving validation workflows enforce consistency for geometry, naming, and structure before integration.'
  },
  {
    title: 'Fix & Optimize',
    body: 'Apply deterministic cleanup and optimization steps so assets remain stable across machines and CI jobs.'
  },
  {
    title: 'Convert & Ship',
    body: 'Export to engine-ready formats and publish artifacts from local scripts, Docker, or GitHub Actions.'
  }
];

export const useCases = [
  {
    title: 'Scan assets in repositories (in progress)',
    body: 'Add repo-wide checks that surface pipeline issues early and support stricter CI gates over time.'
  },
  {
    title: 'Fix and optimize meshes',
    body: 'Repair and optimize imported assets to keep runtime and tooling behavior predictable.'
  },
  {
    title: 'Convert across formats',
    body: 'Move assets between DCC tools and engines with import/export support for 40+ formats.'
  },
  {
    title: 'Merge animation clips',
    body: 'Combine Mixamo, Unreal exports, and DCC clips into clean output files for engine ingestion.'
  },
  {
    title: 'GitHub Actions (Marketplace)',
    body: 'Add `fernandotonon/QtMeshEditor@v1` to any workflow. Scan assets on every PR, convert formats, validate meshes — one line in your YAML.'
  }
];

export const pipelineExamples = {
  scan: `qtmesh scan ./assets --fail-on error\nqtmesh scan ./assets --json --report reports/qtmesh.json`,
  fix: `qtmesh fix model.fbx -o fixed.fbx\nqtmesh fix model.fbx --all -o fixed.fbx`,
  convert: `qtmesh convert model.fbx -o model.glb2\nqtmesh convert model.dae -o model.mesh`,
  merge: `qtmesh anim base.fbx \\\n  --merge walk.fbx run.fbx jump.fbx idle.fbx \\\n  -o merged.fbx`,
  docker: `docker run --rm --user "$(id -u):$(id -g)" -v $(pwd):/workspace \\\n  ghcr.io/fernandotonon/qtmesh scan ./assets --fail-on error`,
  githubAction: `# GitHub Actions Marketplace: fernandotonon/qtmesh\n- uses: fernandotonon/QtMeshEditor@v1\n  with:\n    command: scan\n    input-file: ./assets\n    options: --fail-on warning`,
  scanFixConvert: `qtmesh scan ./assets --fail-on error\nqtmesh fix character.fbx --all -o character_fixed.fbx\nqtmesh convert character_fixed.fbx -o character.glb2`
};

export const mixamoSteps = [
  {
    title: 'Import base + clips',
    body: 'Load a base character and animation clips from Mixamo, Unreal export, or DCC tools.'
  },
  {
    title: 'Merge into one asset',
    body: 'Compose multiple actions into one predictable output file for gameplay integration.'
  },
  {
    title: 'Export to your runtime format',
    body: 'Export to glTF, FBX-compatible workflows, Collada, OBJ, or Ogre Mesh and continue the pipeline.'
  }
];

export const highlightFeatures = [
  {
    title: 'Scene save/load',
    body: 'Store full scenes with meshes, transforms, materials, skeletons, and animations for repeatable edits.'
  },
  {
    title: 'Material tools',
    body: 'Adjust materials visually with realtime feedback during look-dev and technical review.'
  },
  {
    title: 'REST API',
    body: 'Drive mesh and scene operations from external tools and automation scripts.'
  },
  {
    title: 'MCP / AI agent integration',
    body: 'Expose pipeline tools through MCP for advanced scripted and agent-driven workflows.'
  }
];

export const installOptions = [
  {
    platform: 'Windows',
    method: 'winget',
    command: 'winget install FernandoTonon.QtMeshEditor --source winget'
  },
  {
    platform: 'macOS',
    method: 'Homebrew',
    command: 'brew tap fernandotonon/qtmesheditor\nbrew install qtmesheditor'
  },
  {
    platform: 'Linux',
    method: 'snap',
    command: 'sudo snap install qtmesheditor\n# Alternative: download .deb from latest release\n# sudo apt install ./qtmesheditor_amd64.deb'
  },
  {
    platform: 'Docker',
    method: 'Containerized CLI',
    command: 'docker run --rm ghcr.io/fernandotonon/qtmesh --help'
  }
];

export const trustItems = [
  {
    title: 'Open source (MIT)',
    body: 'Use it, inspect it, and contribute improvements directly on GitHub.'
  },
  {
    title: 'Active since 2012',
    body: 'Long-running project with continuous evolution across GUI, CLI, and pipeline tooling.'
  },
  {
    title: 'Pipeline-first by design',
    body: 'Built around practical asset flow outcomes: validate, fix, convert, merge, and automate.'
  }
];

export const footerLinks = [
  { label: 'Documentation', href: links.docs },
  { label: 'GitHub', href: links.github },
  { label: 'Releases', href: links.allReleases },
  { label: 'Issues', href: links.issues },
  { label: 'Ogre3D Forum', href: links.forum },
  { label: 'License (MIT)', href: links.license }
];
