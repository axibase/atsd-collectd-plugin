The ATSD Write plugin sends metrics to [Axibase Time-Series Database](https://axibase.com/products/axibase-time-series-database/).

Synopsis

```
#LoadPlugin write_atsd
#...
#<Plugin write_atsd>
#     <Node "atsd">
#         Host "127.0.0.1"
#         Port 8081
#         Protocol "tcp"
#         <Cache "cpu">
#              Interval 95
#              Threshold 5
#         </Cache>
#         <Cache "df">
#              Interval 300
#              Threshold 0
#         </Cache>
#     </Node>
# </Plugin>
```

Possible settings:

 setting              | required | description                                                                       | default value
----------------------|----------|-----------------------------------------------------------------------------------|----------------
 `Host`      	      | yes      | hostname of target ATSD server                                                                    | `localhost`
 `Port`               | yes      | port of target ATSD server                                                                         | `8081`
 `Protocol`           | yes      | protocol that will be used to transfer data: `tcp` or `udp`                                                      | `tcp`
 `Entity`             | no       | default entity under which all metrics will be stored. By default (if setting is left commented out), entity will be set to the machine hostname. If this setting is uncommented, then the entered value will be used as the entity                                                                    | `hostname`
 `Prefix`             | no       | global prefix for each metric, used to distinguish metrics                                                     | `collectd.`
 `Cache`             | no       | read plugin name to cache: all possible metrics will be counted                                                     | `-`
 `Interval`             | no       | time in seconds that means step between same values                                                     | `-`
 `Threshold`             | no       | allowable deviation threshold from the cached value                                                     | `-`

For example, we can receive the same values (like 0) from read plugins and it will be sent with the default collectd interval (every 10 seconds), but if the value doesn't change we can send it less frequently until it changes. With threshold parameter we can set allowed deviation from previous value (set in percent %). Interval is responsible for frequency at which values (that do not change beyond the threshold) are sent and we can be sure that data points (values) inside this interval are not changing.

Example configuration file that demonstrates to use the main read plugins and their outputs:

```
LoadPlugin aggregation
LoadPlugin contextswitch
LoadPlugin cpu
LoadPlugin df
LoadPlugin disk
LoadPlugin entropy
LoadPlugin interface
LoadPlugin load
LoadPlugin logfile
LoadPlugin memory
LoadPlugin processes
LoadPlugin swap
LoadPlugin syslog
LoadPlugin uptime
LoadPlugin users
LoadPlugin write_atsd

# The following configuration aggregates the CPU statistics from all CPUs
# into one set using the average consolidation function.
<Plugin aggregation>
  <Aggregation>
    Plugin "cpu"
    Type "cpu"
    GroupBy "Host"
    GroupBy "TypeInstance"
    CalculateAverage true
  </Aggregation>
</Plugin>

# The following configuration collects data from all filesystems: 
# the number of free, reserved and used inodes is reported in addition to
# the usual metrics, the values are relative percentage. 
<Plugin df>
    IgnoreSelected true
    ReportInodes true
    ValuesPercentage true
</Plugin>

# The following configuration collects performance statistics from all 
# hard-disks and, where supported, partitions.
<Plugin disk>
    IgnoreSelected true
</Plugin>

# The following configuration collects information about the network traffic,
# packets per second and errors from all network interfaces.
<Plugin interface>
    IgnoreSelected true
</Plugin>

# The following configuration sets the log-level and the file to write
# log messages to; all lines are prefixed by the severity of the log
# message and by the current time.
<Plugin logfile>
    LogLevel info
    File "/var/log/collectd.log"
    Timestamp true
    PrintSeverity true
</Plugin>

# The following configuration sets the log-level info.
<Plugin syslog>
   LogLevel info
</Plugin>

# The following configuration connects to ATSD server on localhost
# via TCP and sends data via port 8081. The data will be sent with
# Entity "entity" and Prefix "collectd".
<Plugin write_atsd>
     <Node "atsd">
         Host "localhost"
         Port 8081
         Protocol "tcp"
         Prefix "collectd."
         Entity "nurswgsvl007"
         <Cache "cpu">
              Interval 95
              Threshold 5
         </Cache>
         <Cache "disk">
              Interval 300
              Threshold 1
         </Cache>
     </Node>
 </Plugin>
```

Commands sent by the ATSD Write plugin to insert time series data into ATSD:

```
series e:nurswgsvl007 ms:1437658049000 m:collectd.cpu.aggregation.idle.average=99.500014
series e:nurswgsvl007 ms:1437658049000 m:collectd.contextswitch.contextswitch=68.128436
series e:nurswgsvl007 ms:1437658049000 m:collectd.cpu.busy=0.301757 t:instance=0
series e:nurswgsvl007 ms:1437658049000 m:collectd.df.space.free=11977220096 t:instance=/
series e:nurswgsvl007 ms:1437658049000 m:collectd.disk.disk_io_time.io_time=17.602089 t:instance=sda
series e:nurswgsvl007 ms:1437658049000 m:collectd.entropy.available=896
series e:nurswgsvl007 ms:1437658049000 m:collectd.interface.if_octets.received=322.393744 t:instance=eth0
series e:nurswgsvl007 ms:1437658049000 m:collectd.load.loadavg.1m=0.08
series e:nurswgsvl007 ms:1437658049000 m:collectd.memory.used=332271616
series e:nurswgsvl007 ms:1437658049000 m:collectd.processes.sleeping=177
series e:nurswgsvl007 ms:1437658049000 m:collectd.memory.swap_used=139268096
series e:nurswgsvl007 ms:1437658049000 m:collectd.uptime.uptime=1185
series e:nurswgsvl007 ms:1437658049000 m:collectd.users.logged_in=4
...
```
