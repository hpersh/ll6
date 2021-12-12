.DEFAULT:	all

all clean check doc:
	for d in bin libs doc; do $(MAKE) -C $$d $@; done
