# QtMeshEditor
A 3D editor for Ogre Mesh made with Qt Framework

### Download and Install Qt: (version: 5.15.2 or Qt6)
	http://qt-project.org/downloads

	You may use the Qt's mingw version.

### Download and Install CMake (Version 3.25.1)
	http://www.cmake.org/cmake/resources/software.html

### Download and Install Git:
	https://git-scm.com/downloads

### Download and Extract Boost: (version: 1.81.0)
	http://www.boost.org/

### Download and install DirectXSDK:
	http://www.microsoft.com/en-us/download/details.aspx?id=6812
	https://archive.org/download/dx81sdk_full/dx81sdk_full.exe

### Clone the QtMeshEditor repository:
	git clone https://github.com/fernandotonon/QtMeshEditor.git

### Clone the Ogre repository: (v. 13.5.3)
	git clone https://github.com/OGRECave/ogre.git

### Clone the Assimp repository:
	git clone https://github.com/assimp/assimp.git


### Verify GCC Version, if it is not right try reinstalling it or ask for help :)
### Tip: use the one provided by Qt

### Build Boost (Windows)
	bootstrap.bat mingw
	b2 toolset=gcc variant=release -j4
	wait some time :)

### Build Ogre
	Open CMake
	Drag and Drop CMakeLists.txt to CMake GUI
	Press Configure
	Select MingW Compiler
	Change CMAKE_BUILD_TYPE to "Release"
	Complete all NOTFOUND components that are essential (You may ask for help if you don't know how to do this step)
	Press Generate
