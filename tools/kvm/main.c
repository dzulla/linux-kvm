#include <stdio.h>

/* user defined header files */
#include <kvm/kvm-cmd.h>

static int handle_kvm_command(int argc, char **argv)
{
	return handle_command(kvm_commands, argc, (const char **) &argv[0]);
}

int main(int argc, char *argv[])
{
	return handle_kvm_command(argc - 1, &argv[1]);
}
