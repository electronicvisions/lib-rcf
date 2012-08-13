#!/usr/bin/env python
import sys, os

def options(opt):
    opt.load('g++')
    opt.load('boost')

def configure(cfg):
    cfg.check_waf_version(mini='1.6.10') # ECM: bleeding EDGE!!1!
    cfg.load('g++')
    cfg.load('boost')

    cfg.check_boost(lib='serialization system thread', uselib_store='BOOST4RCF')

    cfg.env.CXXFLAGS_RCFUSE = [
            '-g',
            '-O0',
            '-DRCF_USE_BOOST_ASIO',
            '-DRCF_USE_BOOST_THREADS',
            '-DRCF_USE_ZLIB',
            '-DRCF_USE_BOOST_SERIALIZATION', # forced by gnu++0x
            '-Wno-deprecated',
            '-fPIC'
    ]
    cfg.env.INCLUDES_RCFUSE   = [ 'include' ]
    cfg.env.LIB_RCFUSE        = [ 'z', 'pthread' ]
    cfg.env.RPATH_RCF         = [ os.path.abspath('lib'), ]

def build(bld):
    bld(
            features        = 'cxx cxxshlib',
            target          = 'rcf',
            source          = 'src/RCF/RCF.cpp',
            use             = 'BOOST4RCF RCFUSE',
            export_includes = 'include',
            install_path    = 'lib',
    )

    bld(
            features        = 'cxx cxxshlib',
            target          = 'sf',
            source          = 'src/SF/SF.cpp',
            use             = 'BOOST4RCF RCFUSE',
            export_includes = 'include',
            install_path    = 'lib',
    )
