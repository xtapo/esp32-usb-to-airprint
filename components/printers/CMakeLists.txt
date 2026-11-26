# Printer drivers (printer_driver.h): one source file per printer family.
idf_component_register(SRCS "printers.c" "canon_pixma.c" "mono_raster.c" "jbig1_enc.c"
                            "hp_laserjet.c" "canon_lbp.c" "brother_laser.c"
                       REQUIRES airprint canon_bjl)
