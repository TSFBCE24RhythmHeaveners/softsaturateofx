OpenFX plugin to render BBB chat recording
==========================================

When recording calls in BBB, you can extract a XML file that contains the
logs of the so called "popcorn chat".

This plugin can read such log and render them on-screen so you can include
them with the video.

This should be compatible with any OpenFX host, but is currently only
tested with Davinci Resolve 18.1


Build
-----

* Make sure to clone this repo using `--recursive` or to init/update
  submodules afterwards since the openFX support libraries are required
  for the build.

* Then build using cmake the usual way.


Install
-------

Currently there is no install target and you need to copy files manually.
On linux the layout should end up looking like :

```
/usr/OFX/Plugins/bbbchatofx.ofx.bundle
/usr/OFX/Plugins/bbbchatofx.ofx.bundle/Contents
/usr/OFX/Plugins/bbbchatofx.ofx.bundle/Contents/Linux-x86-64
/usr/OFX/Plugins/bbbchatofx.ofx.bundle/Contents/Linux-x86-64/bbbchatofx.ofx
```
