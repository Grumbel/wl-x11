wlr_files += files('backend.c')

all_backends = ['x11']  # drm/libinput trees removed in this wl-x11 fork
backends = get_option('backends')
if 'auto' in backends and get_option('auto_features').enabled()
	backends = all_backends
elif 'auto' in backends and get_option('auto_features').disabled()
	backends = []
endif

session_required = 'drm' in backends or 'libinput' in backends or get_option('session').enabled()
if get_option('session').disabled()
	if session_required
		error('Session support is required for the DRM or libinput backends')
	endif
	session_required = disabler()
endif

# session/, drm/, libinput/ trees removed in this wl-x11 fork (X11-only).
# Re-add from upstream if you enable those backends.

foreach backend : all_backends
	if backend in backends or 'auto' in backends
		subdir(backend)
	endif
endforeach

subdir('multi')
subdir('wayland')
subdir('headless')
