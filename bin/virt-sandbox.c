/*
 * virt-sandbox.c: libvirt sandbox command
 *
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <libvirt-sandbox/libvirt-sandbox.h>
#include <glib/gi18n.h>
#include <sys/types.h>
#include <pwd.h>

static gboolean do_close(GVirSandboxConsole *con G_GNUC_UNUSED,
                         gboolean error G_GNUC_UNUSED,
                         gpointer opaque)
{
    GMainLoop *loop = opaque;
    g_main_loop_quit(loop);
    return FALSE;
}

static gboolean do_delayed_close(gpointer opaque)
{
    GMainLoop *loop = opaque;
    g_main_loop_quit(loop);
    return FALSE;
}

static gboolean do_pending_close(GVirSandboxConsole *con G_GNUC_UNUSED,
                                 gboolean error G_GNUC_UNUSED,
                                 gpointer opaque)
{
    GMainLoop *loop = opaque;
    g_timeout_add(2000, do_delayed_close, loop);
    return FALSE;
}

static gboolean do_exited(GVirSandboxConsole *con G_GNUC_UNUSED,
                          int status,
                          gpointer opaque)
{
    int *ret = opaque;
    *ret = WEXITSTATUS(status);
    return FALSE;
}

static void libvirt_sandbox_version(void)
{
    g_print(_("%s version %s\n"), PACKAGE, VERSION);

    exit(EXIT_SUCCESS);
}


int main(int argc, char **argv) {
    int ret = EXIT_FAILURE;
    GVirConnection *hv = NULL;
    GVirSandboxConfig *cfg = NULL;
    GVirSandboxConfigInteractive *icfg = NULL;
    GVirSandboxContext *ctx = NULL;
    GVirSandboxContextInteractive *ictx = NULL;
    GVirSandboxConsole *log = NULL;
    GVirSandboxConsole *con = NULL;
    GMainLoop *loop = NULL;
    GError *error = NULL;
    gchar *name = NULL;
    gchar **disks = NULL;
    gchar **envs = NULL;
    gchar **mounts = NULL;
    gchar **includes = NULL;
    gchar *includefile = NULL;
    gchar *uri = NULL;
    gchar *security = NULL;
    gchar **networks = NULL;
    gchar **cmdargs = NULL;
    gchar *root = NULL;
    gchar *kernver = NULL;
    gchar *kernpath = NULL;
    gchar *kmodpath = NULL;
    gchar *switchto = NULL;
    gboolean verbose = FALSE;
    gboolean debug = FALSE;
    gboolean shell = FALSE;
    gboolean privileged = FALSE;
    GOptionContext *context;
    GOptionEntry options[] = {
        { "version", 'V', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          libvirt_sandbox_version, N_("display version information"), NULL },
        { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
          N_("display verbose information"), NULL },
        { "debug", 'd', 0, G_OPTION_ARG_NONE, &debug,
          N_("display debugging information"), NULL },
        { "connect", 'c', 0, G_OPTION_ARG_STRING, &uri,
          N_("connect to hypervisor"), "URI"},
        { "name", 'n', 0, G_OPTION_ARG_STRING, &name,
          N_("name of the sandbox"), "NAME" },
        { "root", 'r', 0, G_OPTION_ARG_STRING, &root,
          N_("root directory of the sandbox"), "DIR" },
        { "disk", ' ', 0, G_OPTION_ARG_STRING_ARRAY, &disks,
          N_("add a disk in the guest"), "TYPE:TAGNAME=SOURCE,format=FORMAT" },
        { "env", 'e', 0, G_OPTION_ARG_STRING_ARRAY, &envs,
          N_("add a environment variable for the sandbox"), "KEY=VALUE" },
        { "mount", 'm', 0, G_OPTION_ARG_STRING_ARRAY, &mounts,
          N_("mount a filesystem in the guest"), "TYPE:TARGET=SOURCE" },
        { "include", 'i', 0, G_OPTION_ARG_STRING_ARRAY, &includes,
          N_("file to copy into custom dir"), "GUEST-PATH=HOST-PATH", },
        { "includefile", 'I', 0, G_OPTION_ARG_STRING, &includefile,
          N_("file contain list of files to include"), "FILE" },
        { "network", 'N', 0, G_OPTION_ARG_STRING_ARRAY, &networks,
          N_("setup network interface properties"), "PATH", },
        { "security", 's', 0, G_OPTION_ARG_STRING, &security,
          N_("security properties"), "PATH", },
        { "privileged", 'p', 0, G_OPTION_ARG_NONE, &privileged,
          N_("run the command privileged"), NULL },
        { "switchto", 'S', 0, G_OPTION_ARG_STRING, &switchto,
          N_("switch to the given user"), "USER" },
        { "shell", 'l', 0, G_OPTION_ARG_NONE, &shell,
          N_("start a shell"), NULL, },
        { "kernver", 0, 0, G_OPTION_ARG_STRING, &kernver,
          N_("kernel version"), NULL, },
        { "kernpath", 0, 0, G_OPTION_ARG_STRING, &kernpath,
          N_("kernel binary path"), NULL, },
        { "kmodpath", 0, 0, G_OPTION_ARG_STRING, &kmodpath,
          N_("kernel module directory"), NULL, },
        { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &cmdargs,
          NULL, "COMMAND-PATH [ARGS...]" },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };
    const char *help_msg = N_("Run 'virt-sandbox --help' to see a full list of available command line options");
    struct passwd *pw;

    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(PACKAGE, "UTF-8");
    textdomain(PACKAGE);

    if (!gvir_sandbox_init_check(&argc, &argv, &error))
        exit(EXIT_FAILURE);

    g_set_application_name(_("Libvirt Sandbox"));

    context = g_option_context_new (_("- Libvirt Sandbox"));
    g_option_context_add_main_entries (context, options, NULL);
    g_option_context_parse (context, &argc, &argv, &error);
    if (error) {
        g_printerr("%s\n%s\n",
                   error->message,
                   gettext(help_msg));
        goto cleanup;
    }

    g_option_context_free(context);

    if (!cmdargs || (g_strv_length(cmdargs) < 1)) {
        g_printerr(_("\nUsage: %s [OPTIONS] COMMAND-PATH [ARGS...]\n\n%s\n\n"), argv[0], help_msg);
        goto cleanup;
    }

    if (debug) {
        setenv("LIBVIRT_LOG_FILTERS", "1:libvirt", 1);
        setenv("LIBVIRT_LOG_OUTPUTS", "stderr", 1);
    } else if (verbose) {
        setenv("LIBVIRT_LOG_FILTERS", "1:libvirt", 1);
        setenv("LIBVIRT_LOG_OUTPUTS", "stderr", 1);
    }

    loop = g_main_loop_new(g_main_context_default(), FALSE);

    hv = gvir_connection_new(uri);
    if (!gvir_connection_open(hv, NULL, &error)) {
        g_printerr(_("Unable to open connection: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    icfg = gvir_sandbox_config_interactive_new(name ? name : "sandbox");
    cfg = GVIR_SANDBOX_CONFIG(icfg);
    gvir_sandbox_config_interactive_set_command(icfg, cmdargs);

    if (root)
        gvir_sandbox_config_set_root(cfg, root);

    if (kernver)
        gvir_sandbox_config_set_kernrelease(cfg, kernver);
    if (kernpath)
        gvir_sandbox_config_set_kernpath(cfg, kernpath);
    if (kmodpath)
        gvir_sandbox_config_set_kmodpath(cfg, kmodpath);

    if (privileged && switchto) {
        g_printerr(_("'switchto' and 'privileged' are incompatible options\n"));
        goto cleanup;
    }

    if (privileged) {
        gvir_sandbox_config_set_userid(cfg, 0);
        gvir_sandbox_config_set_groupid(cfg, 0);
        gvir_sandbox_config_set_username(cfg, "root");
    } else if (switchto) {
        pw = getpwnam(switchto);
        if (!pw) {
            g_printerr(_("Failed to resolve user %s\n"), switchto);
            goto cleanup;
        }
        gvir_sandbox_config_set_userid(cfg, pw->pw_uid);
        gvir_sandbox_config_set_groupid(cfg, pw->pw_gid);
        gvir_sandbox_config_set_username(cfg, pw->pw_name);
        gvir_sandbox_config_set_homedir(cfg, pw->pw_dir);
    }

    if (envs &&
        !gvir_sandbox_config_add_env_strv(cfg, envs, &error)) {
        g_printerr(_("Unable to parse custom environment variables: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    if (disks &&
        !gvir_sandbox_config_add_disk_strv(cfg, disks, &error)) {
        g_printerr(_("Unable to parse disks: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    if (mounts &&
        !gvir_sandbox_config_add_mount_strv(cfg, mounts, &error)) {
        g_printerr(_("Unable to parse mounts: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    if (includes &&
        !gvir_sandbox_config_add_host_include_strv(cfg, includes, &error)) {
        g_printerr(_("Unable to parse includes: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    if (includefile &&
        !gvir_sandbox_config_add_host_include_file(cfg, includefile, &error)) {
        g_printerr(_("Unable to parse include file: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    if (networks &&
        !gvir_sandbox_config_add_network_strv(cfg, networks, &error)) {
        g_printerr(_("Unable to parse networks: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    if (security &&
        !gvir_sandbox_config_set_security_opts(cfg, security, &error)) {
        g_printerr(_("Unable to parse security: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    if (shell)
        gvir_sandbox_config_set_shell(cfg, TRUE);

    gvir_sandbox_config_set_debug(cfg, debug);
    gvir_sandbox_config_set_verbose(cfg, verbose);

    if (isatty(STDIN_FILENO))
        gvir_sandbox_config_interactive_set_tty(icfg, TRUE);

    ictx = gvir_sandbox_context_interactive_new(hv, icfg);
    ctx = GVIR_SANDBOX_CONTEXT(ictx);

    if (!gvir_sandbox_context_start(ctx, &error)) {
        g_printerr(_("Unable to start sandbox: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    if (!(log = gvir_sandbox_context_get_log_console(ctx, &error))) {
        g_printerr(_("Unable to get log console: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    g_signal_connect(log, "closed", (GCallback)do_close, loop);

    if (!(gvir_sandbox_console_attach_stderr(log, &error))) {
        g_printerr(_("Unable to attach sandbox console: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    if (!(con = gvir_sandbox_context_interactive_get_app_console(ictx, &error))) {
        g_printerr(_("Unable to get app console: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }
    /* We don't close right away - we want to ensure we read any
     * final debug info from the log console. We should get an
     * EOF on that console which will trigger the real close,
     * but we schedule a timer just in case.
     */
    g_signal_connect(con, "closed", (GCallback)do_pending_close, loop);
    g_signal_connect(con, "exited", (GCallback)do_exited, &ret);

    if (!(gvir_sandbox_console_attach_stdio(con, &error))) {
        g_printerr(_("Unable to attach sandbox console: %s\n"),
                   error && error->message ? error->message : _("Unknown failure"));
        goto cleanup;
    }

    g_main_loop_run(loop);

cleanup:
    if (error)
        g_error_free(error);
    if (con)
        gvir_sandbox_console_detach(con, NULL);
    if (ctx) {
        gvir_sandbox_context_stop(ctx, NULL);
        g_object_unref(ctx);
    }
    if (cfg)
        g_object_unref(cfg);
    if (loop)
        g_main_loop_unref(loop);
    if (hv)
        g_object_unref(hv);

    return ret;
}

/*
=head1 NAME

virt-sandbox - Run cmd under a virtual machine sandbox

=head1 SYNOPSIS

virt-sandbox [OPTIONS...] COMMAND

virt-sandbox [OPTIONS...] -- COMMAND [CMDARG1 [CMDARG2 [...]]]

=head1 DESCRIPTION

Run the C<cmd>  application within a tightly confined virtual machine. The
default sandbox domain only allows applications the ability to read and
write stdin, stdout and any other file descriptors handed to it. It is
not allowed to open any other files.

=head1 OPTIONS

=over 8

=item B<-c URI>, B<--connect=URI>

Set the libvirt connection URI, defaults to qemu:///session if
omitted. Alternatively the C<LIBVIRT_DEFAULT_URI> environment
variable can be set, or the config file C</etc/libvirt/libvirt.conf>
can have a default URI set.  Currently only the QEMU and LXC drivers
are supported.

=item B<-n NAME>, B<--name=NAME>

Set the unique name for the sandbox. This defaults to B<sandbox>
but this will need to be changed if more than one sandbox is to
be run concurrently. This is used as the name of the libvirt
virtual machine or container.

=item B<-r DIR>, B<--root DIR>

Use B<DIR> as the root directory of the sandbox, instead of
inheriting the host's root filesystem.

NB. C<DIR> must contain a matching install of the libvirt-sandbox
package. This restriction may be lifted in a future version.

=item B<--env key=value>

Sets up a custom environment variable on a running sandbox.

=item B<--disk TYPE:TAGNAME=SOURCE,format=FORMAT>

Sets up a disk inside the sandbox by using B<SOURCE> with a symlink named as B<TAGNAME>
and type B<TYPE> and format B<FORMAT>. Example: file:cache=/var/lib/sandbox/demo/tmp.qcow2,format=qcow2
Format is an optional parameter.

=over 4

=item B<TYPE>

Type parameter can be set to "file".

=item B<TAGNAME>

TAGNAME will be created under /dev/disk/by-tag/TAGNAME. It will be linked to the device under /dev

=item B<SOURCE>

Source parameter needs to point a file which must be a one of the valid domain disk formats supported by qemu.

=item B<FORMAT>

Format parameter must be set to the same disk format as the file passed on source parameter.
This parameter is optional and the format can be guessed from the image extension

=back

=item B<-m TYPE:DST=SRC>, B<--mount TYPE:DST=SRC>

Sets up a mount inside the sandbox at B<DST> backed by B<SRC>. The
meaning of B<SRC> depends on the value of C<TYPE> specified:

=over 4

=item B<host-bind>

If B<TYPE> is B<host-bind>, then B<SRC> is interpreted as the path
to a directory on the host filesystem. If C<SRC> is the empty string,
then a temporary (empty) directory is created on the host before
starting the sandbox and deleted afterwards. The C<--include> option
is useful for populating these temporary directories with copies of host
files.

=item B<host-image>

If B<TYPE> is B<host-image>, then B<SRC> is interpreted as the path
to a disk image file on the host filesystem. The image should be
formatted with a filesystem that can be auto-detected by the sandbox,
such as B<ext3>, B<ext4>, etc. The disk image itself should be a raw
file, not qcow2 or any other special format

=item B<guest-bind>

If B<TYPE> is B<guest-bind>, then B<SRC> is interpreted as the path
to another directory in the container filesystem.

=item B<ram>

If B<TYPE> is B<ram>, then B<SRC> is interpreted as specifying the
size of the RAM disk in bytes. The suffix B<K>, B<KiB>, B<M>,
B<MiB>, B<G>, B<GiB> can used to alter the units from bytes to a
coarser level.

=back

Some examples

 -m host-bind:/tmp=/var/lib/sandbox/demo/tmp
 -m host-image:/=/var/lib/sandbox/demo.img
 -m guest-bind:/home=/tmp/home
 -m ram:/tmp=500M

=item B<-I HOST-PATH>, B<--includefile=HOST-PATH>

Copy all files listed in inputfile into the
appropriate temporary sandbox directories.

=item B<-N NETWORK-OPTIONS>, B<--network NETWORK-OPTIONS>

Add a network interface to the sandbox. NETWORK-OPTIONS is a set of
key=val pairs, separated by commas. The following options are valid

=over 4

=item dhcp

Configure the network interface using dhcp. This key takes no value.
No other keys may be specified. eg

  -N dhcp,source=default
  --network dhcp,source=lan

where 'source' is the name of any libvirt virtual network.

=item source=NETWORK

Set the name of the network to connect the interface to. C<NETWORK>
is the name of any libvirt virtual network. See also B<virsh net-list>

=item mac=NN:NN:NN:NN:NN:NN

Set the MAC address of the network interface, where each NN is a pair
of hex digits.

=item address=IP-ADDRESS/PREFIX%BROADCAST

Configure the network interface with the static IPv4 or IPv6 address
B<IP-ADDRESS>. The B<PREFIX> value is the length of the network
prefix in B<IP-ADDRESS>. The optional B<BROADCAST> parameter
specifies the broadcast address. Some examples

  address=192.168.122.1/24
  address=192.168.122.1/24%192.168.122.255
  address=2001:212::204:2/64

=item route=IP-NETWORK/PREFIX%GATEWAY

Configure the network interface with the static IPv4 or IPv6 route
B<IP-NETWORK>. The B<PREFIX> value is the length of the network
prefix in B<IP-NETWORK>. The B<GATEWAY> parameter specifies the
address of the gateway for the route. Some examples

  route=192.168.122.255/24%192.168.1.1

=back

=item B<-s SECURITY-OPTIONS>, B<--security=SECURITY-OPTIONS>

Use alternative security options. SECURITY-OPTIONS is a set of key=val pairs,
separated by commas. The following options are valid for SELinux

=over 4

=item dynamic

Dynamically allocate an SELinux label, using the default base context.
The default base context is system_u:system_r:svirt_lxc_net_t:s0 for LXC,
system_u:system_r:svirt_t:s0 for KVM, system_u:system_r:svirt_tcg_t:s0
for QEMU.

=item dynamic,label=USER:ROLE:TYPE:LEVEL

Dynamically allocate an SELinux label, using the base context
USER:ROLE:TYPE:LEVEL, instead of the default base context.

=item static,label=USER:ROLE:TYPE:LEVEL

To set a completely static label. For example,
static,label=system_u:system_r:svirt_t:s0:c412,c355

=item inherit

Inherit the context from the process that is executing virt-sandbox.

=back

=item B<--kernver=VERSION>

Specify the kernel version to run for machine based sandboxes. If
omitted, defaults to match the current running host version.

=item B<--kernpath=FILE-PATH>

Specify the path to the kernel binary. If omitted, defaults to
C</boot/vmlinuz-linux> if exists, otherwise
C</boot/vmlinuz-$KERNEL-VERSION> will be used.

=item B<--kmodpath=DIR-PATH>

Specify the path to the kernel module base directory. If omitted, defaults
to C</lib/modules>. The suffix C<$KERNEL-VERSION/kernel> will be appended
to this path to locate the modules.

=item B<-p>, B<--privileged>

Retain root privileges inside the sandbox, rather than dropping privileges
to match the current user identity.

=item B<-S USER>, B<--switchto=USER>

Switch to the given user inside the sandbox and setup $HOME
accordingly.

=item B<-l>, B<--shell>

Launch an interactive shell on a secondary console device

=item B<-V>, B<--version>

Display the version number and exit

=item B<-v>, B<--verbose>

Display verbose progress information

=item B<-d>, B<--debug>

Display debugging information

=item B<-h>, B<--help>

Display help information

=back

=head1 EXAMPLES

Run an interactive shell under LXC, replace $HOME with the contents
of $HOME/scratch

  # mkdir $HOME/scratch
  # echo "hello" > $HOME/scratch/foo
  # echo "sandbox" > $HOME/scratch/bar
  # virt-sandbox -c lxc:/// -m host-bind:$HOME=$HOME/scratch -i $HOME/scratch/foo -i $HOME/scratch/bar /bin/sh

Convert an OGG file to WAV inside QEMU

  # virt-sandbox -c qemu:///session  -- /usr/bin/oggdec -Q -o - - < somefile.ogg > somefile.wav

=head1 SEE ALSO

C<sandbox(8)>, C<virsh(1)>

=head1 AUTHORS

Daniel P. Berrange <dan@berrange.com>

=head1 COPYRIGHT

Copyright (C) 2011 Daniel P. Berrange <dan@berrange.com>
Copyright (C) 2011-2012 Red Hat, Inc.

=head1 LICENSE

virt-sandbox is distributed under the terms of the GNU LGPL v2+.
This is free software; see the source for copying conditions.
There is NO warranty; not even for MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE

=cut

 */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 *  tab-width: 8
 * End:
 */
