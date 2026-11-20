/*
 * svclook NAME...: whether launchd has each Mach service NAME, the way a client finds it
 * (bootstrap_look_up), one line each. A service a daemon's plist names under a feature flag
 * (#IfFeatureFlagEnabled) is only there when the flag is on: audiomxd spins on one that is not
 * (com.apple.coremedia.endpointmanager.xpc, airplayd's, run 614).
 */
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

/* <servers/bootstrap.h> is not in the SDK this is built with */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *service_name, mach_port_t *sp);

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		mach_port_t port = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_look_up(bootstrap_port, argv[i], &port);

		char line[200];

		/* a kept line ("!!"): a job's output early in a run lands in the log's lost middle */
		snprintf(line, sizeof(line), "!!svclook: %s: %s (%#x)", argv[i], kr == KERN_SUCCESS ? "there" : "NOT THERE",
			 kr);
		sysctlbyname("kern.hakios.say", NULL, NULL, line, strlen(line));
		printf("%s\n", line + 2);
		if (port != MACH_PORT_NULL) {
			mach_port_deallocate(mach_task_self(), port);
		}
	}
	return 0;
}
