All:
	@(make -C third-party)
	@(make -C libai)
	@(make -C coolder)
clean:
	@(make -C third-party clean)
	@(make -C libai clean)
	@(make -C coolder clean)
