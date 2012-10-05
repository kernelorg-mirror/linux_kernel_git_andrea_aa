#!/usr/bin/perl -w
# (c) 2012, Andrew Jones <drjones@redhat.com>
# Licensed under the terms of the GNU GPL License version 2
#
# numa[top]
# Periodically displays system-wide cpu and page node to node migration
# activity. See 'numatop help'. numa is the same except it reads data
# from a file, and then outputs the final cumulative results.
#

use 5.8.2;
use strict;
use warnings;

use lib "$ENV{'PERF_EXEC_PATH'}/scripts/perl/Perf-Trace-Util/lib";
use lib "./Perf-Trace-Util/lib";
use Perf::Trace::Core;
use Perf::Trace::Util;
use POSIX qw/SIGALRM SA_RESTART/;

sub help
{
	print STDERR <<EOF
numatop [options...]
help			- this help
lines=<num>		- [top mode only] change the max number of lines to
			  display (default is 20), use 0 for no limit
delay=<seconds>		- [top mode only] change the refresh interval
			  (default is 3)
migrations|migs		- show per src-dest node counts (outputs > 80 columns)
			  (see Optional output below)
managers		- also show which pid(s) initiated the migration(s)
			  when they're not self initiated. These pids will be
			  displayed below the pid that was migrated and have a
			  '^' flag in the left-most column. Note, it's possible
			  to see the same pid more than once if it manages more
			  than one task.
sort=<criteria>		- select different sort criteria
			  criteria=crit1,crit2,...
			  (default is '<queued>,cmig,tgid,pid',
			   <queued> = qdecay for top mode
			   <queued> = queued for report mode)
			  Any header can be used, plus the below
			    qdecay	- queued, but decayed each interval
			    tgid	- thread group id (Process ID)
			    mptime	- time spent in node-node migrate_pages
			    vmpeak	- peak virtual memory size
			    vmsize	- virtual memory size
mode=<top|report>	- set mode (default is top)
node<nodeid>=<cpulist>	- use one of these for each node to supply the
			  node-cpulist mapping information, e.g.
			  node0=0-3,8-11 node1=4-7,12-15
			  Not necessary for top mode.

Output header:
PID		- Thread ID
PR		- task priority
S		- [top mode only] task status (as in proc)
RSS		- [top mode only] resident set size in MB
N		- [top mode only] NUMA status (requires CONFIG_CPUSETS=y)
		    C - task has cpu affinity to a subset of cpus
		    M - task is bound to a subset of nodes
		    B - task has both cpu affinity and node binding
NODE		- [top mode only] the node last executed on
CMIG		- the number of node-node cpu migrations
PMIG		- the number of node-node page migrations
MIGR		- the number of pages actually migrated
FAIL		- the number of pages that failed to migrate
QUEUED		- the number of pages selected to migrate
COMMAND		- command name

Optional output (migrations):
src>dest_cmig:pmig:queued:%queued ...
  src|?		- the source node or node list (if known, otherwise '?')
  dest|?	- the destination node or node list (if known, otherwise '?')
  cmig		- the number of cpu migrations for this src>dest direction
  pmig		- the number of page migrations for this src>dest direction
  queued	- the number of queued pages for this src>dest direction
  %queued	- the percent of all queued pages selected for this
		  src>dest direction

Live data:
Top mode also outputs the number of cpu migrations per second, the number of
pages migrated per second, the current load average, and, when migration
information is also output (see Optional output), the percentage of used
memory for each node.
EOF
}

my $help = 0;
my $top_mode = 1;
my $nlines = 20;
my $delay = 3;
my $print_migs = 0;
my $print_managers = 0;
my @criteria;
my %sysinfo;
my @report_args;

while (@ARGV) {
	my $arg = shift;
	if ($arg =~ /^help/) {
		$help = 1;
	} elsif ($arg =~ /^lines=(\d+)/) {
		$nlines = $1;
	} elsif ($arg =~ /^delay=(\d+)/) {
		$delay = $1;
	} elsif ($arg =~ /^mig/) {
		$print_migs = 1;
		push @report_args, $arg;
	} elsif ($arg =~ /^managers/) {
		$print_managers = 1;
		push @report_args, $arg;
	} elsif ($arg =~ /^sort=([\w,]+)/) {
		@criteria = split /,/, lc $1;
		push @report_args, $arg;
	} elsif ($arg =~ /^mode=report/) {
		$top_mode = 0;
	} elsif ($arg =~ /^node(\d+)=(.*)/) {
		$sysinfo{cpulists}{$1} = $2;
	}
}
if (!@criteria) {
	@criteria = ('cmig', 'tgid', 'pid');
	if ($top_mode) {
		unshift @criteria, 'qdecay';
	} else {
		unshift @criteria, 'queued';
		push @report_args, 'sort=' . join ',', @criteria;
	}
}
if (!$top_mode) {
	$nlines = 0;
}

my (%procinfo, %data);
my ($runtime, %cpu_to_node);
my (@begin_node, @begin_nodemask);
my $print_pending = 0;

sub trace_begin
{
	if ($help) {
		help();
		exit;
	}

	# change sort criteria to match key names
	my %header_to_key = (
		'tgid'		=> 'Tgid',
		'pid'		=> 'pid',
		'pr'		=> 'prio',
		's'		=> 'State',
		'rss'		=> 'VmRSS',
		'n'		=> 'numa',
		'node'		=> 'node',
		'cmig'		=> 'nr_cpu',
		'pmig'		=> 'nr_page',
		'migr'		=> 'nr_migrated',
		'fail'		=> 'nr_failed',
		'queued'	=> 'nr_queued',
		'command'	=> 'comm',
		'qdecay'	=> 'nr_queued_decay',
		'mptime'	=> 'mp_time',
		'vmpeak'	=> 'VmPeak',
		'vmsize'	=> 'VmSize',
	);

	for (my $i=0; $i <= $#criteria; ++$i) {
		if (defined $header_to_key{$criteria[$i]}) {
			$criteria[$i] = $header_to_key{$criteria[$i]};
		}
	}

	# build the cpu to node map
	get_sysinfo();

	if ($top_mode) {
		# start the timer
		my $sa = POSIX::SigAction->new(\&set_print_pending);
		$sa->flags(SA_RESTART);
		$sa->safe(1);
		POSIX::sigaction(SIGALRM, $sa)
			or die "Can't set SIGALRM handler: $!\n";
		$runtime = 1;
		alarm $runtime;
	}
}

sub trace_end
{
	do_print();
}

sub set_print_pending
{
	$print_pending = 1;
	alarm $delay;
	$runtime += $delay;
}

sub print_check
{
	if ($print_pending == 1) {
		$print_pending = 0;
		do_print();
	}
}

sub sched::sched_migrate_task
{
	my ($event_name, $context, $common_cpu, $common_secs, $common_nsecs,
	    $common_pid, $common_comm, $comm, $pid, $prio, $orig_cpu,
	    $dest_cpu) = @_;

	my $src = cpu_to_node($orig_cpu);
	my $dest = cpu_to_node($dest_cpu);

	# we only care about node to node migrations
	if ($src == $dest) {
		return;
	}

	update_tables($common_pid, $common_comm, $pid, $comm,
		      $prio, "$src>$dest", \&cpumig_update);

	print_check();
}

sub cpumig_update
{
	my $tab = shift;
	$tab->{nr_cpu}++;
}

sub numa::numa_migratepages_nodemask_end
{
	my ($event_name, $context, $common_cpu, $common_secs, $common_nsecs,
	    $common_pid, $common_comm, $nr_failed) = @_;

	end_migratepages(\@begin_nodemask, $common_pid,
			 $common_secs, $common_nsecs, $nr_failed);
}

sub numa::numa_migratepages_end
{
	my ($event_name, $context, $common_cpu, $common_secs, $common_nsecs,
	    $common_pid, $common_comm, $nr_failed) = @_;

	end_migratepages(\@begin_node, $common_pid,
			 $common_secs, $common_nsecs, $nr_failed);
}

sub end_migratepages
{
	my ($begin, $common_pid, $common_secs, $common_nsecs, $nr_failed) = @_;
	my ($secs, $nsecs, $to_migrate);

	if (!@$begin || $begin->[0] != $common_pid) {
		# lost events, kill last begin and skip this end
		@$begin = ();
		return;
	}

	$to_migrate = pop @$begin;
	$nsecs = pop @$begin;
	$secs = pop @$begin;

	if ($secs == $common_secs) {
		$secs = 0;
		$nsecs = $common_nsecs - $nsecs;
	} else {
		$secs = $common_secs - $secs;
		$nsecs = $common_nsecs;
	}

	update_tables(@$begin, \&pagemig_update, $secs, $nsecs,
		      $to_migrate, $nr_failed);
	@$begin = ();

	print_check();
}

sub numa::numa_migratepages_nodemask_begin
{
	my ($event_name, $context, $common_cpu, $common_secs, $common_nsecs,
	    $common_pid, $common_comm, $comm, $pid, $prio, $to_migrate,
	    $masklen, $src, $dest) = @_;
	my $mig;

	if ($print_migs) {
		$mig = rawmask_to_list($src, $masklen) . '>' .
		       rawmask_to_list($dest, $masklen);
	}

	@begin_nodemask = ($common_pid, $common_comm, $pid, $comm, $prio,
			   $mig, $common_secs, $common_nsecs, $to_migrate);
}

sub numa::numa_migratepages_begin
{
	my ($event_name, $context, $common_cpu, $common_secs, $common_nsecs,
	    $common_pid, $common_comm, $comm, $pid, $prio, $to_migrate,
	    $src, $dest) = @_;
	my $mig;

	if ($print_migs) {
		$src = ($src < 0) ? '?' : $src;
		$dest = ($dest < 0) ? '?' : $dest;
		$mig = "$src>$dest";
	}

	@begin_node = ($common_pid, $common_comm, $pid, $comm, $prio,
		       $mig, $common_secs, $common_nsecs, $to_migrate);
}

sub pagemig_update
{
	my ($tab, $secs, $nsecs, $to_migrate, $nr_failed) = @_;

	$tab->{secs} += $secs;
	$tab->{nsecs} += $nsecs;
	$tab->{nr_queued} += $to_migrate;
	$tab->{nr_queued_last} += $to_migrate;

	if ($nr_failed < 0) {
		$tab->{nr_error}++;
		$tab->{last_error} = -$nr_failed;
	} else {
		$tab->{nr_page}++;
		$tab->{nr_failed} += $nr_failed;
		$tab->{nr_migrated} = $tab->{nr_queued} - $tab->{nr_failed};
	}
}

sub do_print
{
	clear_term();
	print_headers();
	print_pids(\%{$data{pids}}, ' ', 0);
	if ($top_mode) {
		set_tasks_need_update();
	}
}

sub print_headers
{
	my ($nr_cpu, $nr_page, $nr_queued,
	    $nr_migrated, $nr_failed) = get_data(\%data);
	my $line_length = 80;

	if ($top_mode) {
		my $runmin = int($runtime/60);
		my $runsecs = $runtime - 60*$runmin;
		printf "numa top - runtime:%5d:%2.2u ".
			"cmig:%7.1f/s queued:%7.1f/s %20s\n",
			$runmin, $runsecs, $nr_cpu/$runtime,
			$nr_queued/$runtime, getloadavg();
	} else {
		print "numa report @report_args\n";
	}

	printf "cmig:%7u pmig:%8u queued:%9u migr:%9u fail:%7u\n",
		$nr_cpu, $nr_page, $nr_queued, $nr_migrated, $nr_failed;

	if ($print_migs) {
		my $optional = '';
		my $migs = '';

		if ($top_mode) {
			$optional = sprintf 'memuse%%: %s', get_mempercents();
		}

		my $gap = 80 - length($optional);
		my $migtotals = 'migtotals:';

		if ($gap > length($migtotals)) {
			$optional .= sprintf "%${gap}s", $migtotals;
		} else {
			$optional .= ' ' . $migtotals;
		}

		$migs = get_migs(\%data);
		print "$optional $migs\n";
#		$line_length += length($migs) + 1;
	}

	printf " %6s %2s %1s %6s %1s %4s %6s %7s %7s/%-5s %7s %-16s\n",
		'PID', 'PR', 'S', 'RSS', 'N', 'NODE', 'CMIG', 'PMIG',
		'MIGR', 'FAIL', 'QUEUED', 'COMMAND';
	print '-' x $line_length . "\n";
}

sub print_pids
{
	my ($pids, $prefix, $line) = @_;
	my $dead;

	foreach my $pid (
		sort { compare($pids, $a, $b, @criteria) } keys %$pids) {

		my $ptab = \%{$pids->{$pid}};
		$dead = 0;

		if ($top_mode) {
			update_task($pid);
			$dead = task_is_dead($pid);
		}

		if (!$dead && (!$nlines || ++$line <= $nlines)) {

			print_one_pid($ptab, $prefix);

			if ($print_migs) {
				printf " %s", get_migs($ptab);
			}
			print "\n";

			if ($print_managers && defined $ptab->{managers}) {
				my $managers = $ptab->{managers};
				$line = print_pids($managers, '^', $line);
			}
		}

		if ($dead) {
			delete_task($pids, $pid);
		} elsif ($top_mode) {
			decay_queued($ptab);
		}
	}

	return $line;
}

sub print_one_pid
{
	my ($ptab, $prefix) = @_;
	my ($nr_cpu, $nr_page, $nr_queued, $nr_migrated, $nr_failed,
	    $pid, $comm, $prio, $state, $rss, $numa, $node) = get_data($ptab);
	my $fmt = '%1s%6u %2s %1s ';
	$fmt .= ($rss eq '-') ? '%6s' : '%6u';
	$fmt .= ' %1s ';
	$fmt .= ($node eq '-') ? '%4s' : '%4u';
	$fmt .= ' %6u %7u ';

	if (!$nr_page) {
		$fmt .= '%7s/%-5s %7s';
		$nr_queued = $nr_migrated = $nr_failed = '-';
	} else {
		$fmt .= '%7u/%-5u %7u';
	}
	$fmt .= ' %-16s';

	$rss = ($rss eq '-') ? $rss : int($rss/1024);

	printf $fmt, $prefix, $pid, $prio, $state, $rss, $numa, $node,
	       $nr_cpu, $nr_page, $nr_migrated, $nr_failed, $nr_queued, $comm;

	if ($ptab->{nr_error}) {
		printf ' migrate_pages failed %u times, last error = %u',
			$ptab->{nr_error}, $ptab->{last_error};
	}
}

sub get_migs
{
	my $ptab = shift;
	my $migs = \%{$ptab->{migs}};
	my $ret = '';
	my $sep = '';

	foreach my $mig (
		sort { compare($migs, $a, $b, ('mig')) } keys %$migs) {

		my ($nr_cpu, $nr_page, $nr_queued) =
					get_data(\%{$migs->{$mig}});
		my $percent = 0;
		my $fmt = '%s%s_%u:%u:%u:%u';

		if ($nr_page) {
			$percent = int(100 * $nr_queued/$ptab->{nr_queued});
		}
		$ret .= sprintf $fmt, $sep, $mig, $nr_cpu, $nr_page,
				$nr_queued, $percent;
		$sep = ' ';
	}
	return $ret;
}

sub decay_queued
{
	my $ptab = shift;

	if (!defined $ptab->{nr_queued_decay}) {
		my $div = $ptab->{nr_queued} || 0;
		$ptab->{nr_queued_decay} = int($div/2);
	} else {
		$ptab->{nr_queued_decay} >>= 1; # int div by 2
	}

	$ptab->{nr_queued_decay} += $ptab->{nr_queued_last} || 0;
	$ptab->{nr_queued_last} = 0;
}

sub update_tables
{
	my ($common_pid, $common_comm, $pid, $comm, $prio, $mig,
	    $update, @args) = @_;

	if (!$pid) {
		# without $pid info we assume we're self managed, but
		# acknowledge that $prio is undefined
		$pid = $common_pid;
		$comm = $common_comm;
		$prio = undef;
	}

	# list of tables to update
	my @tables = (\%data, \%{$data{pids}{$pid}});

	$data{pids}{$pid}{pid} = $pid;
	$procinfo{$pid}{comm} = $comm;
	$procinfo{$pid}{prio} = set_prio($prio);
	$procinfo{$pid}{refcnt}++;

	if ($print_managers && $common_pid != $pid) {

		# we're managed by another pid, track it too
		my $managers = \%{$data{pids}{$pid}{managers}};
		push @tables, \%{$managers->{$common_pid}};

		$managers->{$common_pid}{pid} = $common_pid;
		$procinfo{$common_pid}{comm} = $common_comm;
		$procinfo{$common_pid}{refcnt}++;

	}

	if ($print_migs) {
		# add the mig table for each table to the list
		my @tables2;
		foreach my $tab (@tables) {
			push @tables2, ($tab, \%{$tab->{migs}{$mig}});
		}
		@tables = @tables2;
	}

	# finally update each table
	foreach my $tab (@tables) {
		&$update($tab, @args);
	}
}

sub set_prio
{
	my $prio = shift;
	if (!defined $prio) {
		return '--';
	}
	$prio -= 100;
	return ($prio < 0) ? 'RT' : $prio;
}

sub compare
{
	my ($tab, $a, $b, @crits) = @_;
	my $crit = shift @crits;
	my $ret = 0;

	if (!defined $crit) {
		return 0;
	}

	if ($crit eq 'pid') {
		$ret = ($a <=> $b);
	} elsif ($crit eq 'mig') {
		my ($src_a, $dest_a) = split />/, $a;
		my ($src_b, $dest_b) = split />/, $b;

		# push unknowns and masks to the back
		if ($src_a eq '?' || $src_a =~ /[-,]/ ||
		    $dest_a eq '?' || $dest_a =~ /[-,]/) {
			return 1;
		}
		if ($src_b eq '?' || $src_b =~ /[-,]/ ||
		    $dest_b eq '?' || $dest_b =~ /[-,]/) {
			return -1;
		}
		$ret = ($src_a <=> $src_b || $dest_a <=> $dest_b);
	} elsif ($crit eq 'mp_time') {
		# reverse sort by time
		my ($secs_a, $nsecs_a, $secs_b, $nsecs_b);
		$secs_a = $tab->{$a}{secs} || 0;
		$secs_b = $tab->{$b}{secs} || 0;
		$nsecs_a = $tab->{$a}{nsecs} || 0;
		$nsecs_b = $tab->{$b}{nsecs} || 0;
		$ret = ($secs_b <=> $secs_a || $nsecs_b <=> $nsecs_a);
	} elsif (defined $tab->{$a}{$crit} || defined $tab->{$b}{$crit}) {
		# reverse sort by the given counter
		my $nr_a = $tab->{$a}{$crit} || 0;
		my $nr_b = $tab->{$b}{$crit} || 0;
		$ret = ($nr_b <=> $nr_a);
	} elsif (defined $procinfo{$a}{$crit} || defined $procinfo{$a}{$crit}) {
		my $val_a = $procinfo{$a}{$crit} || 0;
		my $val_b = $procinfo{$b}{$crit} || 0;
		if ($crit =~ /^Vm/) {
			# reverse numeric sort
			$ret = ($val_b <=> $val_a);
		} elsif ($crit eq 'Tgid' || $crit eq 'prio') {
			# numeric compare
			$ret = ($val_a <=> $val_b);
		} else {
			# string compare
			$val_a = $val_a || '';
			$val_b = $val_b || '';
			$ret = ($val_a cmp $val_b);
		}
	}
	return ($ret || compare($tab, $a, $b, @crits));
}

sub get_data
{
	my $tab = shift;
	my (@data, $pid);

	$data[0] = $tab->{nr_cpu}		|| 0;
	$data[1] = $tab->{nr_page}		|| 0;
	$data[2] = $tab->{nr_queued}		|| 0;
	$data[3] = $tab->{nr_migrated}		|| 0;
	$data[4] = $tab->{nr_failed}		|| 0;

	$pid = $tab->{pid};
	if (!defined $pid) {
		return @data;
	}
	$data[5] = $pid;
	$data[6] = $procinfo{$pid}{comm}	|| '';
	$data[7] = $procinfo{$pid}{prio}	|| '--';
	$data[8] = $procinfo{$pid}{State}	|| '-';
	$data[9] = (defined $procinfo{$pid}{VmRSS})
			? $procinfo{$pid}{VmRSS} : '-';
	$data[10] = $procinfo{$pid}{numa}	|| '-';
	$data[11] = (defined $procinfo{$pid}{node})
			? $procinfo{$pid}{node}	: '-';

	return @data;
}

sub get_sysinfo
{
	if ($top_mode) {
		my ($sys, $h);

		# get lists of all available nodes and cpus
		$sys = '/sys/devices/system/node/online';
		open $h, "<$sys" or
			die "can't open $sys, not a NUMA system?: $!";
		chomp($sysinfo{nodelist} = <$h>);
		close $h;
		push @{$sysinfo{nodes}}, list_expand($sysinfo{nodelist});

		$sys = '/sys/devices/system/cpu/online';
		open $h, "<$sys" or die "can't open $sys: $!";
		chomp($sysinfo{cpulist} = <$h>);
		close $h;

		# get per-node cpulists
		if (!defined $sysinfo{cpulists}) {
			$sys = '/sys/devices/system/node';
			foreach my $node (@{$sysinfo{nodes}}) {
				my $f = sprintf "$sys/node%d/cpulist", $node;
				open $h, "<$f" or die "can't open $f: $!";
				chomp(my $cpulist = <$h>);
				close $h;
				$sysinfo{cpulists}{$node} = $cpulist;
			}
		}
	}

	if (!defined $sysinfo{cpulists}) {
		die <<EOF
No node cpu lists found!
If running in report mode, then make sure the lists are passed on the command
line, e.g. node0=0-3,8-11 node1=4-7,12-15
If running in top mode, then make sure the system is NUMA.
EOF
	}

	# build the cpu_to_node map
	foreach my $node (keys %{$sysinfo{cpulists}}) {
		my @cpus = list_expand($sysinfo{cpulists}{$node});
		foreach my $cpu (@cpus) {
			$cpu_to_node{$cpu} = $node;
		}
	}
}

sub list_expand
{
	my $list = shift;
	my @expanded;
	foreach my $seq (split /,/, $list) {
		if ($seq =~ /-/) {
			my ($start, $end) = split /-/, $seq;
			foreach my $i ($start..$end) {
				push @expanded, $i;
			}
		} else {
			push @expanded, $seq;
		}
	}
	return @expanded;
}

sub cpu_to_node
{
	my $cpu = shift;
	if (defined $cpu_to_node{$cpu}) {
		return $cpu_to_node{$cpu};
	} elsif ($top_mode) {
		my $dir = sprintf '/sys/devices/system/cpu/cpu%d', $cpu;
		opendir my $dh, $dir or die "can't open $dir: $!";
		my @node = grep { /^node/ && s/node// } readdir $dh;
		closedir $dh;
		$cpu_to_node{$cpu} = $node[0];
		return $node[0];
	} else {
		print STDERR "ERROR: cpu_to_node: no node for $cpu!\n";
	}
	return 0;
}

sub rawmask_to_list
{
	my ($mask, $len) = @_;
	my @bytes = unpack("C$len", $mask);
	my (@bits, @list);
	my ($start, $end);

	for (my $byte = 0; $byte <= $#bytes; ++$byte) {
		for (my $i = 0; $i < 8; ++$i) {
			if ($bytes[$byte] & (1 << $i)) {
				push @bits, $byte*8 + $i;
			}
		}
	}

	for (my $i = 0; $i <= $#bits; ++$i) {
		$start = $end = $bits[$i];
		while ($i < $#bits && (($bits[$i] + 1) == $bits[$i + 1])) {
			$end = $bits[++$i];
		}
		if ($start == $end) {
			push @list, "$start";
		} else {
			push @list, "$start-$end";
		}
	}

	return (@list) ? join ',', @list : '?';
}

sub getloadavg
{
	open my $h, '</proc/loadavg' or die "can't read proc: $!";
	my @loads = split / /, <$h>;
	close $h;
	return join ' ', $loads[0], $loads[1], $loads[2];
}

sub get_mempercents
{
	my %memused;
	my $total = 0;
	my $ret = '';
	my $sep = '';

	foreach my $node (@{$sysinfo{nodes}}) {
		my $f = sprintf "/sys/devices/system/node/node%d/meminfo",
				$node;
		open my $h, "<$f" or die "can't open $f: $!";
		my @a = grep { /MemUsed/ } <$h>;
		close $h;
		@a = split /\s+/, $a[0];
		$memused{$node} = $a[3];
		$total += $memused{$node};
	}
	foreach my $node (@{$sysinfo{nodes}}) {
		$ret .= sprintf "%s%u:%u", $sep, $node,
			int(($memused{$node} * 100)/$total);
		$sep = ' ';
	}
	return $ret;
}

sub set_tasks_need_update
{
	foreach my $pid (keys %procinfo) {
		$procinfo{$pid}{updated} = 0;
	}
}

sub update_task
{
	my $pid = shift;
	if (!$procinfo{$pid}{updated}) {
		__update_task($pid);
		$procinfo{$pid}{updated} = 1;
	}
}

sub delete_task
{
	my ($tab, $pid) = @_;
	delete $tab->{$pid};
	if (--$procinfo{$pid}{refcnt} <= 0) {
		delete $procinfo{$pid};
	}
}

sub task_is_dead
{
	my $pid = shift;
	return (defined $procinfo{$pid}{State}
			&& $procinfo{$pid}{State} eq 'X');
}

sub __update_task
{
	my $pid = shift;
	my ($fh, $cpus, $mems, $stat, @stat);

	if ($pid == 0) {
		$procinfo{$pid}{State} = '-';
		return;
	}

	open $fh, "</proc/$pid/status" or goto dead;
	foreach my $line (<$fh>) {
		my ($key, $val) = split /\s+/, $line;
		chomp($val);
		if ($key =~ /^State/) {
			$procinfo{$pid}{State} = $val;
		} elsif ($key =~ /^Tgid/) {
			$procinfo{$pid}{Tgid} = $val;
		} elsif ($key =~ /^VmPeak/) {
			$procinfo{$pid}{VmPeak} = $val;
		} elsif ($key =~ /^VmSize/) {
			$procinfo{$pid}{VmSize} = $val;
		} elsif ($key =~ /^VmRSS/) {
			$procinfo{$pid}{VmRSS} = $val;
		} elsif ($key =~ /^Cpus_allowed_list/) {
			$cpus = $val;
		} elsif ($key =~ /^Mems_allowed_list/) {
			$mems = $val;
		}
	}
	close $fh;

	$cpus = (defined $cpus && $cpus ne $sysinfo{cpulist});
	$mems = (defined $mems && $mems ne $sysinfo{nodelist});
	if ($cpus && $mems) {
		$procinfo{$pid}{numa} = 'B';
	} elsif ($cpus) {
		$procinfo{$pid}{numa} = 'C';
	} elsif ($mems) {
		$procinfo{$pid}{numa} = 'M';
	} else {
		$procinfo{$pid}{numa} = '-';
	}

	open $fh, "</proc/$pid/stat" or goto dead;
	$stat = <$fh>;
	close $fh;
	$stat =~ s/.*\) //; # get past command name which may have spaces
	@stat = split / /, $stat; # now 'state' is at @stat index zero

	$procinfo{$pid}{prio} = ($stat[15] < 0) ? 'RT' : $stat[15];
	$procinfo{$pid}{node} = cpu_to_node($stat[36]);

	return;
dead:
	$procinfo{$pid}{State} = 'X';
}
