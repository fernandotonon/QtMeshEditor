
# <img width=30 align="top" src="https://user-images.githubusercontent.com/996529/209745977-7b797223-46ce-4bce-aa70-707a88f2aaf2.png"> QtMeshEditor
A graphical editor for Ogre3D mesh and material made with Qt Framework

[![GitHub stars](https://img.shields.io/github/stars/fernandotonon/QtMeshEditor.svg?style=social&label=Star&maxAge=2592000)](https://GitHub.com/fernandotonon/QtMeshEditor/stargazers) Star if you like it! 

[![Github All Releases](https://img.shields.io/github/downloads/fernandotonon/QtMeshEditor/total.svg)]()
[![Deploy](https://github.com/fernandotonon/QtMeshEditor/actions/workflows/deploy.yml/badge.svg)](https://github.com/fernandotonon/QtMeshEditor/actions/workflows/deploy.yml)
[![Coverage](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=coverage)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Maintainability Rating](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_rating)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Vulnerabilities](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=vulnerabilities)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Technical Debt](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=sqale_index)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)
[![Duplicated Lines (%)](https://sonarcloud.io/api/project_badges/measure?project=fernandotonon_QtMeshEditor&metric=duplicated_lines_density)](https://sonarcloud.io/summary/new_code?id=fernandotonon_QtMeshEditor)

### :movie_camera: Preview

![QtMeshEditor1 5 0](https://user-images.githubusercontent.com/996529/210196572-7b49da4c-c5db-406d-9ab4-7fa20bacb6ae.gif)

#### Skeleton Animation Controls

![QtMeshEditor1 6 0](https://user-images.githubusercontent.com/996529/218779819-0a61156d-c014-4ad1-aa8b-cee900c9da56.gif)

### :sparkles: Features
* Mesh
  - Translation, Scale, and Rotation
  - Change the material of the mesh - Allows the change of the mesh material
  - Primitives creation, using ogre-procedural - Easy tool to create Box, Sphere, and Plane
* Material
  - Shows in real time the material changes on the model
  - Material can be edited using GUI or code editor
* Skeleton
  - View the bones
  - Animation Preview - Shows a list of animations and allows the animation preview.
  - Renaming Animation
  - View keyframe positions and values
* Import/Export
  - Export mesh in older versions 
  - Other 3D Format Importer - Can convert any 3D format provided by ASSIMP to Ogre Mesh, Material, and Skeleton
  - Other 3D Format Exporter - Can export the mesh to any other format supported by ASSIMP.

### :computer: Install from release binaries
#### :apple: macOS
##### Homebrew
`brew tap fernandotonon/qtmesheditor`

`brew install qtmesheditor`

Remove with

`brew remove qtmesheditor`

Upgrade with

`brew upgrade qtmesheditor`

<img width="502" alt="image" src="https://github.com/fernandotonon/QtMeshEditor/assets/996529/84f56be3-4522-45a7-9039-5a143de7313c">


##### DMG file
* Download and open the .dmg file;
* Drag and drop the QtMeshEditor to the Applications folder:

![install_macOS](https://user-images.githubusercontent.com/996529/216797862-2592a40b-5f3d-4907-bcad-dc1feae4ff2f.gif)

#### :penguin: Linux (ubuntu)

Download the .deb file;
Then there are a few options for installing it:
* Using apt
`sudo apt install ./qtmesheditor_amd64.deb`

* Using dpkg
`sudo dpkg -i qtmesheditor_amd64.deb`

* Using the Software Install:
<img width="600" src="https://user-images.githubusercontent.com/996529/216799515-51494c16-c420-4535-a898-5b915a340c88.png">

Remove with 

`sudo apt remove qtmesheditor`

or

`sudo dpkg -r qtmesheditor`

Run it calling `qtmesheditor`

#### :window: Windows
Unpack the binaries and run the `QtMeshEditor.exe` file

### [Build-from-source](https://github.com/fernandotonon/QtMeshEditor/wiki/How-to-build)

Feel free to contact me, create issues or to contribute ;)
