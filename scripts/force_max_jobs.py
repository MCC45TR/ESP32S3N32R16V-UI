import os

from SCons.Script import GetOption, SetOption


max_jobs = os.cpu_count() or 1

# Always use the host maximum core count for parallel build jobs.
SetOption("num_jobs", int(max_jobs))

print(
    "[build] force_max_jobs: num_jobs={} (host cores={})".format(
        GetOption("num_jobs"), max_jobs
    )
)
