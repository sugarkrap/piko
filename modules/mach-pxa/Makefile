
obj-y				+= devices.o generic.o irq.o reset.o
obj-$(CONFIG_PM)		+= pm.o sleep.o standby.o

obj-$(CONFIG_PXA25x)		+= mfp-pxa2xx.o pxa2xx.o pxa25x.o
obj-$(CONFIG_PXA27x)		+= mfp-pxa2xx.o pxa2xx.o pxa27x.o
obj-$(CONFIG_PXA3xx)		+= mfp-pxa3xx.o pxa3xx.o smemc.o
obj-$(CONFIG_CPU_PXA300)	+= pxa300.o
obj-$(CONFIG_CPU_PXA320)	+= pxa320.o

obj-$(CONFIG_MACH_PXA25X_DT)	+= pxa-dt.o
obj-$(CONFIG_MACH_PXA27X_DT)	+= pxa-dt.o
obj-$(CONFIG_MACH_PXA3XX_DT)	+= pxa-dt.o

obj-$(CONFIG_ARCH_GUMSTIX)	+= gumstix.o
obj-$(CONFIG_GUMSTIX_AM200EPD)	+= am200epd.o
obj-$(CONFIG_GUMSTIX_AM300EPD)	+= am300epd.o

obj-$(CONFIG_PXA_SHARP_C7xx)	+= corgi.o sharpsl_pm.o corgi_pm.o
obj-$(CONFIG_PXA_SHARP_Cxx00)	+= spitz.o sharpsl_pm.o spitz_pm.o
