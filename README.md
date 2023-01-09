
# <img width=30 align="top" src="https://user-images.githubusercontent.com/996529/209745977-7b797223-46ce-4bce-aa70-707a88f2aaf2.png"> QtMeshEditor
A 3D editor for Ogre Mesh made with Qt Framework

### Preview

![QtMeshEditor1 5 0](https://user-images.githubusercontent.com/996529/210196572-7b49da4c-c5db-406d-9ab4-7fa20bacb6ae.gif)


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

### Clone the Ogre Procedural:
	git clone https://github.com/OGRECave/ogre-procedural.git

### Clone the Assimp repository:
	git clone https://github.com/assimp/assimp.git


### Verify GCC Version, if it is not right try reinstalling it or ask for help :)
### Tip: use the one provided by Qt

### Build Boost (Windows)
	bootstrap.bat mingw
	b2 toolset=gcc variant=release -j4
	wait some time :)

### Add BOOST_ROOT to the system environment variables
	BOOST_ROOT=C:/Boost (for windows)

### Build Assimp
	Configure and Generate using CMake
	mingw32-make install

### Add ASSIMP_HOME to the system environment variables
	ASSIMP_HOME=C:/Program Files (x86)/Assimp (for windows, or the path configured in the CMAKE_INSTALL_PREFIX)	

### Build Ogre
	Open CMake
	Drag and Drop CMakeLists.txt to CMake GUI
	For Linux, set OGRE_GLSUPPORT_USE_EGL to FALSE (it craches on trying to use multiple viewports)
	Press Configure
	Select MingW Compiler
	Make sure the Assimp_DIR is configured (C:/Assimp/lib/cmake/assimp-5.2)
	Complete all NOTFOUND components that are essential (You may ask for help if you don't know how to do this step)
	Press Generate
	mingw32-make install

### Build Ogre Procedural

### Add OgreProcedural_HOME to the system environment variables

### Build QtMeshEditor
