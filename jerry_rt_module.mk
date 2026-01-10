################################################################################
#
# jerry_rt_module
#
################################################################################

JERRY_RT_MODULE_VERSION = 1.0
JERRY_RT_MODULE_SITE = $(JERRY_RT_MODULE_PKGDIR)
JERRY_RT_MODULE_SITE_METHOD = local
JERRY_RT_MODULE_LICENSE = GPL-2.0
JERRY_RT_MODULE_LICENSE_FILES = 

# This is a kernel module
JERRY_RT_MODULE_MODULE_SUBDIRS = .

# Use kernel-module infrastructure
$(eval $(kernel-module))
$(eval $(generic-package))
