SUBDIRS = src

.PHONY: all clean
.DEFAULT: all

all clean:
	@echo " [$@] "
	@for dir in $(SUBDIRS) ; do \
		$(MAKE) $(SILENT) -C $$dir $@ || exit 2; done
