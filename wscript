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

    cfg.env.DEFINES_RCFUSE = [
            'RCF_USE_BOOST_ASIO',
            'RCF_USE_BOOST_THREADS',
            'RCF_USE_ZLIB',
            'RCF_USE_BOOST_SERIALIZATION', # forced by gnu++0x
    ]
    cfg.env.LIB_RCFUSE        = [ 'z', 'pthread' ]
    cfg.env.RPATH_RCF         = [ os.path.abspath('lib'), ]

    # just for SF
    cfg.check_boost(lib='system thread', uselib_store='BOOST4RCFSF')
    cfg.env.DEFINES_RCFUSESF = [
            'RCF_USE_BOOST_ASIO',
            'RCF_USE_BOOST_THREADS',
            'RCF_USE_ZLIB',
            'RCF_USE_SF_SERIALIZATION', # ECM: What about c++0x?
    ]
    cfg.env.LIB_RCFUSESF        = [ 'z', 'pthread' ]

def build(bld):
    inc = bld.path.find_dir('include').abspath()
    flags = { "cxxflags"  : ['-g', '-O0', '-Wno-deprecated'],
              "linkflags" : ['-Wl,-z,defs'],
              "includes"  : [inc],
              }

    bld(
            features        = 'cxx cxxshlib',
            target          = 'rcf',
            source          = 'src/RCF/RCF.cpp',
            use             = 'BOOST4RCF RCFUSE',
            export_includes = inc,
            install_path    = 'lib',
            **flags
    )

    bld(
            features        = 'cxx cxxshlib',
            target          = 'sf',
            idx             = 123, # ECM: same source file...
            source          = 'src/RCF/RCF.cpp',
            use             = 'BOOST4RCFSF RCFUSESF',
            export_includes = inc,
            install_path    = 'lib',
            **flags
    )
