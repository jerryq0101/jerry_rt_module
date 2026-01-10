IRQ_BUTTON_DEMO_VERSION = 1.0
# The right name for the configuration
IRQ_BUTTON_DEMO_SITE = $(IRQ_BUTTON_DEMO_PKGDIR)
IRQ_BUTTON_DEMO_SITE_METHOD = local
IRQ_BUTTON_DEMO_LICENSE = GPL-2.0
IRQ_BUTTON_DEMO_LICENSE_FILES = 

# This is a kernel module
IRQ_BUTTON_DEMO_MODULE_SUBDIRS = .

# Use kernel-module infrastructure
$(eval $(kernel-module))
$(eval $(generic-package))
