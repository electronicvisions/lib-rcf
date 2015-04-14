#!/usr/bin/env python
import os, copy


def options(opt):
    opt.load('g++')
    opt.load('boost')


def configure(cfg):
    cfg.check_waf_version(mini='1.6.10') # ECM: bleeding EDGE!!1! (needed for multiple boost checks below)
    cfg.load('g++')
    cfg.load('boost')

    cfg.check_cfg(package='zlib', uselib_store='ZLIB')
    cfg.check_boost(lib='system thread', uselib_store='BOOST4RCF')
    cfg.check_boost(lib='serialization system thread', uselib_store='BOOST4RCF_WSERIALIZATION')
    cfg.check_boost(lib='filesystem serialization system thread', uselib_store='BOOST4RCF_WSERIALIZATION_WFS')

    DEFINES_common = [
        'RCF_USE_BOOST_ASIO',
        'RCF_USE_BOOST_THREADS',
        'RCF_USE_ZLIB',
        'RCF_NO_AUTO_INIT_DEINIT',
    ]
    cfg.env.DEFINES_RCFSF      = DEFINES_common + [ 'RCF_USE_SF_SERIALIZATION' ]
    cfg.env.DEFINES_RCFBOOST   = DEFINES_common + [ 'RCF_USE_BOOST_SERIALIZATION' ]
    cfg.env.DEFINES_RCFBOOSTSF = DEFINES_common + [ 'RCF_USE_BOOST_SERIALIZATION', 'RCF_USE_SF_SERIALIZATION' ]

    LIB_common = [ 'z', 'pthread' ]
    cfg.env.LIB_RCFSF = LIB_common
    cfg.env.LIB_RCFBOOST = LIB_common
    cfg.env.LIB_RCFBOOSTSF = LIB_common


def build(bld):
    inc = bld.path.find_dir('include').abspath()
    common_flags = { "cxxflags"  : ['-g', '-O0', '-Wno-deprecated'],
              'linkflags' : ['-Wl,-z,defs'],
              'includes'  : [inc],
              'defines'   : [],
              'use'       : [], #['RCFUSE'],
    }

    bld(
        target          = 'rcf_inc',
        export_includes = inc
    )

    # TODO: ugly target names, but for backwards compatibility
    for i,s in enumerate(['rcf', 'sf', 'rcfsf', 'rcf_fs']):
        flags = copy.deepcopy(common_flags)
        if s == 'sf': # pure sf
            flags['use'].append('BOOST4RCF')
            flags['use'].append('RCFSF')
        if s == 'rcf': # pure boost
            flags['use'].append('BOOST4RCF_WSERIALIZATION')
            flags['use'].append('RCFBOOST')
        if s == 'rcf_fs': # pure boost with filesystem support
            flags['use'].append('BOOST4RCF_WSERIALIZATION_WFS')
            flags['use'].append('RCFBOOST')
            flags['defines'] += ['RCF_USE_BOOST_FILESYSTEM', 'BOOST_FILESYSTEM_DEPRECATED']
        if s == 'rcfsf': # both
            flags['use'].append('BOOST4RCF_WSERIALIZATION')
            flags['use'].append('RCFBOOSTSF')

        bld(
                features        = 'cxx cxxshlib',
                target          = s,
                idx             = i,
                source          = 'src/RCF/RCF.cpp',
                export_includes = inc,
                install_path    = '${PREFIX}/lib',
                **flags
        )
