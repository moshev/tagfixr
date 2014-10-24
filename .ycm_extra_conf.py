def FlagsForFile(file, **kwargs):
    return {
        'flags': '-std=c11 -Wall -Werror -D__USE_POSIX=1'.split(),
        'do_cache': False,
    }

