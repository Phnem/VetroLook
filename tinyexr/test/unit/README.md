# Build&Test

## Prepare

Clone `https://github.com/openexr/openexr-images` to `../../../` directory.
(Or edit path to openexr-images in `tester.cc`)

## V1 API (Stable Release)

The original TinyEXR API - stable and production-ready.

### Use makefile

    $ make check

### Use ninja + kuroga

Assume

* ninja 1.4+
* python 2.6+

Are installed.

#### Linux/MacOSX

    $ python kuroga.py config-posix.py
    $ ninja

#### Windows

    > python kuroga.py config-msvc.py
    > vcbuild.bat

## V2 API (Deprecated)

The experimental V2 API and its old unit tests have moved to `../../attic/` for
reference only. They are no longer part of the active unit-test suite.
