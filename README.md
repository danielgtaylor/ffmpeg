FFmpeg Readme
=============

This is FFmpeg + libavfilter + several patches to make using FFmpeg easier. 

Installation
------------

Use the following commands to get, build, and install FFmpeg.

    git clone git://github.com/danielgtaylor/ffmpeg.git
    cd ffmpeg
    git submodule init
    git submodule update
    ./configure ... --enable-avfilter --enable-avfilter-lavf ...
    make -j4
    make install

Documentation
-------------

  * Read the documentation in the doc/ directory.

Licensing
---------

   * See the LICENSE file.

