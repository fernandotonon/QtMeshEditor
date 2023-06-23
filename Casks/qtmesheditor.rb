cask 'qtmesheditor' do
  version '1.7.2'
  sha256 '42ea67a503efac05b8b6c827d6c381525cfb948d7cf263d2a1f528b2c9e3b261'

  url "https://github.com/fernandotonon/QtMeshEditor/releases/download/#{version}/QtMeshEditor-#{version}-MacOS.dmg"
  name 'QtMeshEditor'
  homepage 'https://github.com/fernandotonon/QtMeshEditor'
  desc 'Qt-based Ogre3D Mesh Editor'

  app 'QtMeshEditor.app'
end
