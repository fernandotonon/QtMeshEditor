export const links = {
  github: 'https://github.com/fernandotonon/QtMeshEditor',
  docs: './docs.html',
  qtmeshCloud: 'https://qtmesh.dev',
  qtmeshCloudApi: 'https://api.qtmesh.dev',
  releases: 'https://github.com/fernandotonon/QtMeshEditor/releases/latest',
  allReleases: 'https://github.com/fernandotonon/QtMeshEditor/releases',
  issues: 'https://github.com/fernandotonon/QtMeshEditor/issues',
  forum: 'https://forums.ogre3d.org/viewtopic.php?t=76016',
  license: 'https://opensource.org/license/mit',
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
  title: 'Catch 3D asset issues before they break your game.',
  subtitle:
    'QtMeshEditor helps developers and technical artists lint, validate, fix, and track 3D asset pipelines across GUI, CLI, Docker, GitHub Actions, and QtMesh Cloud reporting.',
  ctaPrimary: 'Download QtMeshEditor',
  ctaSecondary: 'View on GitHub',
  ctaDocs: 'Documentation'
};

export const proofPoints = [
  'CI/CD for 3D assets',
  'GUI + CLI + Docker',
  'GitHub Actions + QtMesh Cloud',
  'Open source (MIT)'
];

export const pipelineFlow = [
  {
    title: 'Scan',
    body: 'Scan repository assets for format, naming, and structural issues before integration.'
  },
  {
    title: 'Validate',
    body: 'Apply quality rules consistently so imports and runtime behavior stay predictable.'
  },
  {
    title: 'Fix & Optimize',
    body: 'Run deterministic cleanup and optimization to reduce pipeline drift across environments.'
  },
  {
    title: 'Convert & Ship',
    body: 'Convert and publish engine-ready artifacts through local scripts, Docker, and CI.'
  }
];

export const beforeAfter = {
  before: [
    'Broken animations discovered late',
    'Missing materials after import',
    'Inconsistent naming across teams',
    'Pipeline surprises near release'
  ],
  after: [
    'Validated assets at every push',
    'Predictable imports and outputs',
    'CI visibility for asset quality',
    'Shareable quality badges for stakeholders'
  ]
};

export const audienceCards = [
  {
    title: 'Indie game developers',
    body: 'Catch asset regressions early and keep production moving without building custom tooling from scratch.',
    tag: 'Indie'
  },
  {
    title: 'Technical artists / pipeline owners',
    body: 'Standardize checks, automate fixes, and keep DCC-to-engine handoffs consistent across contributors.',
    tag: 'Technical Art'
  },
  {
    title: 'Studios with CI/CD workflows',
    body: 'Add quality gates for 3D assets like code, with historical tracking and visible health signals in CI.',
    tag: 'Studio CI/CD'
  }
];

export const pipelineExamples = {
  scan: `qtmesh scan ./assets --fail-on error\nqtmesh scan ./assets --json --report reports/qtmesh.json`,
  fix: `qtmesh fix model.fbx -o fixed.fbx\nqtmesh fix model.fbx --all -o fixed.fbx`,
  convert: `qtmesh convert model.fbx -o model.glb2\nqtmesh convert model.dae -o model.mesh`,
  merge: `qtmesh anim base.fbx \\\n  --merge walk.fbx run.fbx jump.fbx idle.fbx \\\n  -o merged.fbx`,
  docker: `docker run --rm --user "$(id -u):$(id -g)" -v $(pwd):/workspace \\\n  ghcr.io/fernandotonon/qtmesh scan ./assets --fail-on error`,
  githubAction: `name: QtMesh Scan\n\non:\n  push:\n    branches: [ "master" ]\n\njobs:\n  scan-assets-qtmesh:\n    runs-on: ubuntu-latest\n    steps:\n      - uses: actions/checkout@v4\n\n      - name: Run QtMesh scan\n        uses: __QTMESH_ACTION_REF__\n        with:\n          command: scan\n        env:\n          QTMESH_CLOUD_TOKEN: \${{ secrets.QTMESH_CLOUD_TOKEN }}`,
  scanFixConvert: `qtmesh scan ./assets --fail-on error\nqtmesh fix character.fbx --all -o character_fixed.fbx\nqtmesh convert character_fixed.fbx -o character.glb2`
};

export const cloudBadgeSteps = [
  {
    title: 'Connect a project',
    body: 'Create a QtMesh Cloud project and store a project token in CI secrets.'
  },
  {
    title: 'Upload scan results on every run',
    body: 'Send scan JSON to /v1/ingest/scan to keep branch and commit quality history.'
  },
  {
    title: 'Publish live badges',
    body: 'Expose status, score, errors, and warnings with badge URLs tied to your project.'
  }
];

export const cloudBadgeMarkdown = `[![qtmesh status](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-status.svg)](https://qtmesh.dev)
[![qtmesh errors](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-errors.svg)](https://qtmesh.dev)
[![qtmesh warnings](https://api.qtmesh.dev/v1/u/<owner-slug>/p/<project-slug>/badges/qtmesh-warnings.svg)](https://qtmesh.dev)`;

export const cloudBadgeUploadExample = `- name: Scan assets
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
      --data-binary @qtmesh-scan-upload.json`;

export const mixamoSteps = [
  {
    title: 'Import base + clips',
    body: 'Bring in Mixamo, Unreal, and DCC clips around a shared rig base.'
  },
  {
    title: 'Merge into one asset',
    body: 'Merge clips into one predictable output for gameplay and content review.'
  },
  {
    title: 'Export to your runtime format',
    body: 'Export to glTF/FBX/Collada/Ogre Mesh and feed directly into CI and engine import.'
  }
];

export const highlightFeatures = [
  {
    title: 'Scene save/load',
    body: 'Store full scenes with meshes, transforms, materials, skeletons, and animations.'
  },
  {
    title: 'Material tools',
    body: 'Adjust materials visually with realtime feedback for look-dev and QA.'
  },
  {
    title: 'REST API',
    body: 'Drive mesh and scene operations from external tools and automation scripts.'
  },
  {
    title: 'MCP / AI agent integration',
    body: 'Expose pipeline tools through MCP for scripted and agent-driven workflows.'
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
    body: 'Long-running project with steady evolution across editor, CLI, and CI workflows.'
  },
  {
    title: 'Pipeline-first by design',
    body: 'Built around real asset outcomes: validate, fix, convert, merge, and automate.'
  }
];

export const footerLinks = [
  { label: 'Documentation', href: links.docs },
  { label: 'QtMesh Cloud', href: links.qtmeshCloud },
  { label: 'GitHub', href: links.github },
  { label: 'Releases', href: links.allReleases },
  { label: 'Issues', href: links.issues },
  { label: 'Ogre3D Forum', href: links.forum },
  { label: 'License (MIT)', href: links.license }
];
