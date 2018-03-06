#!/usr/bin/env python
import os, copy


def options(opt):
    opt.load('compiler_cxx')
    opt.load('boost')


def configure(cfg):
    cfg.check_waf_version(mini='1.6.10') # ECM: bleeding EDGE!!1! (needed for multiple boost checks below)
    cfg.load('compiler_cxx')
    cfg.load('boost')

    cfg.check_cxx(lib="pthread", uselib_store="PTHREAD")
    cfg.check_cfg(package='zlib', args='--libs --cflags')
    cfg.check_boost(lib='system', uselib_store='BOOST4RCF')
    cfg.check_boost(lib='serialization system', uselib_store='BOOST4RCF_WSERIALIZATION')
    cfg.check_boost(lib='filesystem serialization system', uselib_store='BOOST4RCF_WSERIALIZATION_WFS')


    cfg.env.DEFINES_RCF_COMMON     = ['RCF_USE_ZLIB']
    cfg.env.DEFINES_RCF_SF_ONLY    = ['RCF_USE_SF_SERIALIZATION'] # automatically defined
    cfg.env.DEFINES_RCF_BOOST_ONLY = ['RCF_USE_BOOST_SERIALIZATION']
    cfg.env.DEFINES_RCF_SF_BOOST   = ['RCF_USE_BOOST_SERIALIZATION',
                                      'RCF_USE_SF_SERIALIZATION']
    cfg.env.DEFINES_RCF_BOOST_FS   = ['RCF_USE_BOOST_FILESYSTEM',
                                      'BOOST_FILESYSTEM_DEPRECATED']

    cfg.check_cxx(lib="dl", uselib_store="DL4RCF", mandatory=True)


def build(bld):
    inc = bld.path.find_dir('include').abspath()
    common_flags = {
              'linkflags' : [],
              'includes'  : [inc],
              'cxxflags'  : [],
              'defines'   : [],
              'use'       : [
                  'RCF_COMMON',
                  'PTHREAD',
                  'ZLIB',
                  'DL4RCF'
                  ],
    }

    bld(
        target          = 'rcf_inc',
        export_includes = inc
    )

    # TODO: ugly target names, but for backwards compatibility
    # TODO: fix rcf-boost-fs target if it is needed in the future --obreitwi, 23-02-18 14:37:25
    for i,s in enumerate(['rcf-sf-only', 'rcf-boost-only', 'rcf-sf-boost']):
        flags = copy.deepcopy(common_flags)
        if s == 'rcf-sf-only': # pure sf
            flags['use'].extend([
                'BOOST4RCF',
                'RCF_SF_ONLY'
                ])
        if s == 'rcf-boost-only': # pure boost
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION',
                'RCF_BOOST_ONLY'
                ])
        if s == 'rcf-boost-fs': # pure boost with filesystem support
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION_WFS',
                'RCF_BOOST_ONLY'
                ])
            flags['use'].append('BOOST4RCF_WSERIALIZATION_WFS')
        if s == 'rcf-sf-boost': # both
            flags['use'].extend([
                'BOOST4RCF_WSERIALIZATION',
                'RCF_SF_BOOST'
                ])

        bld(
                features        = 'cxx cxxshlib',
                target          = s,
                idx             = i,
                source          = 'src/RCF/RCF.cpp',
                export_includes = inc,
                install_path    = '${PREFIX}/lib',
                **flags
        )
