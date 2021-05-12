# QtMeshEditor
A 3D editor for Ogre Mesh made with Qt Framework

### Download and Install Qt: (version: 5.2.1)
	http://qt-project.org/downloads

	You may use the Qt's mingw version.

### Download and Install CMake (Version 2.8.12.2)
	http://www.cmake.org/cmake/resources/software.html

### Download and Install TortoiseHg: (version: 2.11.0)
	http://mercurial.selenic.com/wiki/Download

### Download and Install TortoiseGIT: (version: 1.8.7)
	http://code.google.com/p/tortoisegit/

### Download and Extract Boost: (version: 1.55.0)
	http://www.boost.org/

### Download and Install masm32: (version: 11) -- To build boost
	http://www.masm32.com/masmdl.htm
	After installing you have to include the bin folder to the system variable: Path

### Download and install DirectXSDK:
	http://www.microsoft.com/en-us/download/details.aspx?id=6812

### Clone the QtMeshEditor repository:
	https://bitbucket.org/fernandot/qtmesheditor

### Clone the Ogre repository:
	https://bitbucket.org/sinbad/ogre

### Clone the Ogredeps repository:
	https://bitbucket.org/cabalistic/ogredeps

### Clone the Assimp repository:
	https://github.com/assimp/assimp.git

### Clone the OgreAssimp repository:
	https://bitbucket.org/jacmoe/ogreassimp

### Verify GCC Version, if it is not right try reinstalling it or ask for help :)

### Build Boost
	bootstrap.bat mingw
	b2 toolset=gcc variant=release -j3
	wait some time :)


### Build Ogredeps
	Open CMake
	Drag and Drop CMakeLists.txt to CMake GUI
	Press Configure again
	Press Generate
	Open command
	Enter into the folder and type "mingw32-make install"
	Set the OGRE_DEPENDENCIES_DIR into the system variables

### Build Ogre
	Open command
	type "hg checkout v1-9" on the ogre's folder
	Open CMake
	Drag and Drop CMakeLists.txt to CMake GUI
	Press Configure
	Select MingW Compiler
	Change CMAKE_BUILD_TYPE to "Release"
	Complete all NOTFOUND components that are essential (You may ask for help if you don't know how to do this step)
	Press Generate
