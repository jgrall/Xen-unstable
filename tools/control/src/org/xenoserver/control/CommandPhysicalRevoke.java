package org.xenoserver.control;

/**
 * Revoke physical access to a partition from a domain.
 */
public class CommandPhysicalRevoke extends Command {
    /** Defaults instance to use. */
    private Defaults d;
    /** Domain to revoke access from */
    private int domain_id;
    /** Partition to revoke access to */
    private String partition_name;
    /** Number to substitute for + (-1 => use domain_id) */
    private int subst;

    /**
     * Constructor for CommandPhysicalRevoke.
     * @param d Defaults object to use.
     * @param domain_id Domain to revoke access from.
     * @param partition Partition to revoke access to.
     */
    public CommandPhysicalRevoke(Defaults d, int domain_id, String partition, int subst) {
        this.d = d;
        this.domain_id = domain_id;
        this.partition_name = partition;
	this.subst = subst;
    }

    /**
     * @see org.xenoserver.control.Command#execute()
     */
    public String execute() throws CommandFailedException {
        Runtime r = Runtime.getRuntime();
        String output = null;
	String resolved = StringPattern.parse(partition_name).resolve(subst == -1 ? domain_id : subst);
	String resolved2 = d.runCommand(d.xiToolsDir + Settings.XI_HELPER + " expand " + resolved).trim();
        Partition partition = PartitionManager.IT.getPartition(resolved2);

        if (partition == null) {
          throw new CommandFailedException("Partition " + partition_name + " (resolved to " + resolved2 + ") does not exist.");
        }

        try {
            Process start_p;
            String start_cmdarray[] = new String[5];
            int start_rc;
            start_cmdarray[0] = d.xiToolsDir + "xi_phys_revoke";
            start_cmdarray[1] = Integer.toString(domain_id);
            Extent e = partition.toExtent();
            start_cmdarray[2] = Integer.toString(e.getDisk());
            start_cmdarray[3] = Long.toString(e.getOffset());
            start_cmdarray[4] = Long.toString(e.getSize());

            if (Settings.TEST) {
                output = reportCommand(start_cmdarray);
            } else {
                start_p = r.exec(start_cmdarray);
                start_rc = start_p.waitFor();
                if (start_rc != 0) {
                    throw CommandFailedException.xiCommandFailed(
                        "Could not revoke physical access",
                        start_cmdarray);
                }
                output = "Revoked physical access from domain " + domain_id;
            }
        } catch (CommandFailedException e) {
            throw e;
        } catch (Exception e) {
            throw new CommandFailedException(
                "Could not revoke physical access (" + e + ")",
                e);
        }

        return output;
    }

}
