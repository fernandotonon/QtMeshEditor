export const links = {
  github: 'https://github.com/fernandotonon/QtMeshEditor',
  releases: 'https://github.com/fernandotonon/QtMeshEditor/releases/latest',
  allReleases: 'https://github.com/fernandotonon/QtMeshEditor/releases',
  issues: 'https://github.com/fernandotonon/QtMeshEditor/issues',
  forum: 'https://forums.ogre3d.org/viewtopic.php?t=76016',
  license: 'https://opensource.org/license/mit',
  actions: 'https://github.com/fernandotonon/QtMeshEditor/tree/master/.github/actions/qtmesh'
};

export const media = {
  mergeDemo: {
    src: 'https://github.com/user-attachments/assets/441f90c5-1968-4838-8001-4ca24856a501',
    alt: 'QtMeshEditor animation merge workflow with imported FBX assets'
  },
  skeletonPreview: {
    src: 'https://github.com/user-attachments/assets/289403ac-8952-488c-bc65-0a768ab278e1',
    alt: 'Bone weight visualization inside QtMeshEditor'
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
  title: 'Merge Mixamo animations and ship 3D assets faster.',
  subtitle:
    'QtMeshEditor is a focused GUI + CLI toolkit for indie teams: merge animation clips, convert 40+ formats, inspect skeletons, and automate repeatable asset tasks.',
  ctaPrimary: 'Download Latest',
  ctaSecondary: 'View on GitHub'
};

export const proofPoints = ['Free & open source', 'Windows / macOS / Linux', 'GUI + CLI', '40+ formats'];

export const mergeSteps = [
  {
    title: 'Load files',
    body: 'Import your base character and animation clips from Mixamo, Unreal export, or DCC tools.'
  },
  {
    title: 'Merge animations',
    body: 'Select clips and merge them into one clean character file in seconds.'
  },
  {
    title: 'Export to your engine',
    body: 'Export to glTF, FBX-compatible workflows, Collada, OBJ, or Ogre Mesh and keep moving.'
  }
];

export const useCases = [
  {
    title: 'Merge animations',
    body: 'Combine separate animation clips into one asset to simplify import and iteration.'
  },
  {
    title: 'Convert formats',
    body: 'Move assets across DCC tools and engines with import/export support for 40+ formats.'
  },
  {
    title: 'Inspect skeletons and weights',
    body: 'Audit bone hierarchies, inspect skinning, and catch rig problems before runtime.'
  },
  {
    title: 'Automate with qtmesh CLI',
    body: 'Run conversions, merges, and checks from scripts, CI jobs, and build pipelines.'
  }
];

export const pipelineExamples = {
  inspect: `qtmesh info model.fbx
qtmesh info model.fbx --json`,
  convert: `qtmesh convert model.fbx -o model.gltf2
qtmesh convert model.dae -o model.mesh`,
  merge: `qtmesh anim base.fbx \\
  --merge walk.fbx run.fbx jump.fbx idle.fbx \\
  -o merged.fbx`,
  docker: `docker run --rm --user "$(id -u):$(id -g)" -v $(pwd):/workspace \\
  ghcr.io/fernandotonon/qtmesh convert model.fbx -o model.gltf2`,
  githubAction: `- uses: fernandotonon/QtMeshEditor/.github/actions/qtmesh@master
  with:
    command: anim
    input-file: assets/base.fbx
    options: --merge assets/walk.fbx assets/run.fbx -o assets/merged.fbx`
};

export const comparisonItems = [
  {
    title: 'GUI for manual work',
    body: 'Visual editing for skeleton fixes, material tweaks, and export validation.'
  },
  {
    title: 'CLI for automation',
    body: 'Scriptable commands for repeatable conversion and merge jobs.'
  },
  {
    title: 'Docker + Actions for pipelines',
    body: 'Containerize qtmesh and run asset steps inside GitHub Actions CI/CD.'
  }
];

export const highlightFeatures = [
  {
    title: 'Scene save/load',
    body: 'Store complete scenes with meshes, transforms, materials, skeletons, and animations.'
  },
  {
    title: 'Material editor',
    body: 'Adjust material values with realtime visual feedback during look-dev.'
  },
  {
    title: 'AI-assisted materials',
    body: 'Generate or refine material setups quickly when exploration speed matters.'
  },
  {
    title: 'MCP / AI agent integration',
    body: 'Expose editor tools through MCP for advanced scripted agent workflows.'
  },
  {
    title: 'REST API',
    body: 'Drive mesh and scene operations from external tools and automation scripts.'
  },
  {
    title: 'Multi-platform distribution',
    body: 'Available via winget, Homebrew, snap, .deb packages, Docker, and release binaries.'
  }
];

export const installOptions = [
  {
    platform: 'Windows',
    method: 'WinGet',
    command: 'winget install FernandoTonon.QtMeshEditor'
  },
  {
    platform: 'macOS',
    method: 'Homebrew',
    command: 'brew tap fernandotonon/qtmesheditor\nbrew install qtmesheditor'
  },
  {
    platform: 'Linux',
    method: 'Snap / .deb',
    command: 'sudo snap install qtmesheditor\n# or: sudo apt install ./qtmesheditor_amd64.deb'
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
    body: 'Long-running project with continuous evolution across GUI, CLI, and formats.'
  },
  {
    title: 'Indie-friendly',
    body: 'Built for practical day-to-day asset work in small teams and solo projects.'
  }
];

export const footerLinks = [
  { label: 'GitHub', href: links.github },
  { label: 'Releases', href: links.allReleases },
  { label: 'Issues', href: links.issues },
  { label: 'Ogre3D Forum', href: links.forum },
  { label: 'License (MIT)', href: links.license }
];
