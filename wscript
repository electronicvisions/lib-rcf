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

    # just for SF
    cfg.check_boost(lib='system thread', uselib_store='BOOST4RCFSF')
    cfg.env.CXXFLAGS_RCFUSESF = [
            '-g',
            '-O0',
            '-DRCF_USE_BOOST_ASIO',
            '-DRCF_USE_BOOST_THREADS',
            '-DRCF_USE_ZLIB',
            '-DRCF_USE_SF_SERIALIZATION', # ECM: What about c++0x?
            '-Wno-deprecated',
            '-fPIC'
    ]
    cfg.env.INCLUDES_RCFUSESF   = [ 'include' ]
    cfg.env.LIB_RCFUSESF        = [ 'z', 'pthread' ]

def build(bld):
    inc = bld.path.find_dir('include').abspath()
    bld(
            features        = 'cxx cxxshlib',
            target          = 'rcf',
            source          = 'src/RCF/RCF.cpp',
            use             = 'RCFUSE BOOST4RCF',
            export_includes = inc,
            install_path    = 'lib',
    )

    bld(
            features        = 'cxx cxxshlib',
            target          = 'sf',
            idx             = 123, # ECM: same source file...
            cxxflags        = '-fPIC',
            source          = 'src/RCF/RCF.cpp',
            use             = 'RCFUSESF BOOST4RCFSF',
            export_includes = inc,
            install_path    = 'lib',
    )
