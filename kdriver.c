#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/unistd.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/paravirt.h>
#include <asm/uaccess.h>
#include <linux/errno.h>

#include "kdriver.h"

#define PROC_V    "/proc/version"
#define BOOT_PATH "/boot/System.map-"
#define MAX_VERSION_LEN   256

struct virus_def *vdef;
struct white_list_data *head = NULL;
unsigned long *syscall_table = NULL;

asmlinkage long (*original_open) (const char __user *, int, umode_t);
asmlinkage long (*original_execve) (const char __user *filename, const char __user *const __user *argv, const char __user *const __user *envp);


/* This method retreives the system-call table address in memory */
static int get_system_call_table(char *kern_ver)
{
	char system_map_entry[MAX_VERSION_LEN];
	int i = 0;
	
	    /*
	     * Holds the /boot/System.map-<version> file name while it's been built
	     */ 
	char *filename;
	
	    /*
	     * Length of the System.map filename, terminating NULL included
	     */ 
	 size_t filename_length = strlen(kern_ver) + strlen(BOOT_PATH) + 1;
	
	    /*
	     * This will points to /boot/System.map-<version> file
	     */ 
	struct file *f = NULL;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	printk(KERN_EMERG "Kernel version: %s", kern_ver);
	filename = kmalloc(filename_length, GFP_KERNEL);

	if (filename == NULL) {
		printk(KERN_EMERG
			"kmalloc failed on System.map-<version> filename allocation");
		return -1;
	}
	 
	    /*
	     * Zero out memory to be safe
	     */ 
	    memset(filename, 0, filename_length);
	 
	    /*
	     * Construct our /boot/System.map-<version> file name
	     */ 
	    strncpy(filename, BOOT_PATH, strlen(BOOT_PATH));
	    strncat(filename, kern_ver, strlen(kern_ver));
	 
	    /*
	     * Open the System.map file for reading
	     */ 
	    f = filp_open(filename, O_RDONLY, 0);
	if (IS_ERR(f) || (f == NULL)) {
		printk(KERN_EMERG
			"Error opening System.map-<version> file: %s\n", filename);
		return -1;
	}
	 memset(system_map_entry, 0, MAX_VERSION_LEN);
	 
	    /*
	     * Read one byte at a time from the file until we either max out
	     * out our buffer or read an entire line.
	     */ 
	    while (vfs_read(f, system_map_entry + i, 1, &f->f_pos) == 1) {
		
		    /*
		     * If we've read an entire line or maxed out our buffer,
		     * check to see if we've just read the sys_call_table entry.
		     */ 
		    if (system_map_entry[i] == '\n' || i == MAX_VERSION_LEN) {
			
			    // Reset the "column"/"character" counter for the row
			    i = 0;
			 if (strstr(system_map_entry, "sys_call_table") !=
			       NULL) {
				char *sys_string;
				char *system_map_entry_ptr = system_map_entry;
				 sys_string =
				    kmalloc(MAX_VERSION_LEN, GFP_KERNEL);
				if (sys_string == NULL) {
					filp_close(f, 0);
					set_fs(oldfs);
					kfree(filename);
					 return -1;
				}
				 memset(sys_string, 0, MAX_VERSION_LEN);
				
				    /* copy path upto max_len */ 
				    strncpy(sys_string,
					    strsep(&system_map_entry_ptr, " "),
					    MAX_VERSION_LEN);
				
				    /* value is written in hex format */ 
				    kstrtoul(sys_string, 16, (unsigned long *)&syscall_table);
				printk(KERN_EMERG "syscall_table retrieved\n");
				 kfree(sys_string);
				break;
			}
			    memset(system_map_entry, 0, MAX_VERSION_LEN);
			continue;
		}
		i++;
	}
	filp_close(f, 0);
	set_fs(oldfs);
	kfree(filename);
	return 0;
}


/*
 * We have to pass in a pointer to a buffer to store the parsed
 * version information in. If we declare a pointer to the
 * parsed version info on the stack of this function, the
 * pointer will disappear when the function ends and the
 * stack frame is removed.
 */
char *acquire_kernel_version(char *buf)
{
	struct file *proc_version;
	char *kernel_version;
	
	    /*
	     * We use this to store the userspace perspective of the filesystem
	     * so we can switch back to it after we are done reading the file
	     * into kernel memory
	     */ 
	    mm_segment_t oldfs;
	
	    /*
	     * Standard trick for reading a file into kernel space
	     * This is very bad practice. We're only doing it here because
	     * we're malicious and don't give a damn about best practices.
	     */ 
	    oldfs = get_fs();
	set_fs(KERNEL_DS);
	
	    /*
	     * Open the version file in the /proc virtual filesystem
	     */ 
	    proc_version = filp_open(PROC_V, O_RDONLY, 0);
	if (IS_ERR(proc_version) || (proc_version == NULL)) {
	       return NULL;
	}
	    /*
	     * Zero out memory just to be safe
	     */ 
	    memset(buf, 0, MAX_VERSION_LEN);
	    /*
	     * Read version info from /proc virtual filesystem
	     */ 
	    vfs_read(proc_version, buf, MAX_VERSION_LEN,
		     &(proc_version->f_pos));
	
	    /*
	     * Extract the third field from the full version string
	     */ 
	kernel_version = strsep(&buf, " ");
	kernel_version = strsep(&buf, " ");
	kernel_version = strsep(&buf, " ");
	filp_close(proc_version, 0);
	    /*
	     * Switch filesystem context back to user space mode
	     */ 
	    set_fs(oldfs);
	return kernel_version;
}

bool is_file_malicious(const char *path){
	char *kpath;
	struct file *filp;
	struct file_data *fdata;
	struct inode *input_file_inode = NULL;
	umode_t input_file_mode;
	long err = 0;
	bool ret_val = false;
	bool is_malicious = false;
	bool is_renamed = false;
	kpath = get_path_name(path);
	if (kpath == NULL) {
		printk(KERN_ERR "KDRIVER: could not get path from user\n");
		err = -ENOMEM;
		is_malicious = true;
		goto out;
	}
    	
  	printk("\nKpath = %s",kpath);

	filp = filp_open(kpath, O_RDONLY, 0);
	if (filp == NULL || IS_ERR(filp)) {
		printk(KERN_ERR "Cannot open file\n");
		/* if kernel cant open this file then user also cant, hence false*/
		is_malicious = false;
		goto out;
	}
	/* checking if the file is regular, if file is not regular invoking original_open*/
	input_file_inode = file_inode(filp);
	input_file_mode = input_file_inode->i_mode;
	if(!S_ISREG(input_file_mode)){
		printk(KERN_INFO "Not a regular file");
		goto out;
	}

	fdata = create_file_data_struct(filp);
	if (fdata == NULL) {
		printk(KERN_ERR
			"error occured while reading from file to scan\n");
		/* if kernel cant read from this file then user also cant, hence false*/
		is_malicious = false;
		goto out_vdef;
	}

	/* checking if the file is white listed*/
	ret_val = is_white_listed(filp, fdata);
	if(ret_val){
		printk("\nFile is white listed!");
		goto out_vdef;
	}
	else{
		printk("\nFile is not white listed.. checking for blacklist!");
        }

	/* moving vdef here since we should not read virus definitions if file is white listed*/
        if (vdef == NULL) {
               vdef = read_virus_def();
               if (vdef == NULL){
	               goto out_close;
		}
        }

	err = scan(filp, fdata, vdef);

	if (err > 0){
	  printk("\nFile contains virus\n");
	  printk("\nRenaming file to .virus");
	  is_malicious = true;
	  is_renamed = rename_malicious_file (kpath);
	  if(is_renamed)
		printk("\nRenamed file to .virus");
	  else
		printk("\nCouldn't rename file to .virus");
	}
	
	kfree(fdata);
	fdata = NULL;
 out_vdef:
	kfree(vdef);
	vdef = NULL;
 out_close:
	if (filp && !IS_ERR(filp))
		filp_close(filp, NULL);
 out:
	if(kpath){	
		kfree(kpath);
		kpath = NULL;
	}
	return is_malicious;
}

asmlinkage long new_open(const char __user * path, int flags, umode_t mode)
{
	bool is_malicious = false;

	if(flags  <= 32768){
		is_malicious = is_file_malicious(path);
		printk("\nIn kernel flags = %d",flags);
	}

	if(!is_malicious)
		return original_open(path, flags, mode);
	else
		return -EACCES; 
}

asmlinkage long new_execve(const char __user *filename, const char __user *const __user *argv, const char __user *const __user *envp)
{
        bool is_malicious = false;
	is_malicious = is_file_malicious(filename);
	printk("\nIntercepted exexc");
        if(!is_malicious)
		return original_execve(filename, argv, envp);
	else
		return -EACCES;
}

bool read_white_list(void) {
	struct file *whitelistdb = NULL;
	struct white_list_data *iterator = head, *node = NULL; 
	mm_segment_t oldfs;
	char * buffer = NULL, *buffer_orig = NULL;
	int bytes_read = 0, i = 0;
	
	/* return value */
	bool err = true;

	whitelistdb = filp_open(WHITELIST_DB_FILE, O_RDONLY, 0);
        if (whitelistdb == NULL || IS_ERR(whitelistdb)) {
                printk(KERN_ERR "cannot open whitelist\n");
		err = false;
                goto out;
        }
	buffer = kmalloc(sizeof(char) * 4059, GFP_KERNEL);
	if(buffer == NULL){
		printk("\nCould not allocate memory for whitelist definitions");
		err = false;
		goto out;	
	}
	buffer_orig = buffer;

	/* adding each signature of whitelist into a linked list*/
	do{
	buffer = buffer_orig;
	buffer[0] = '\0';
        oldfs = get_fs ();
	set_fs (KERNEL_DS);
        bytes_read = vfs_read (whitelistdb, buffer, 4059, &whitelistdb->f_pos);	
        set_fs (oldfs);
	if(bytes_read < 0){
		printk("\nCouldn't read white list into buffer");
		err = false;
		goto out;
	}

	i = 0;
        while(i < bytes_read){
	        node = kmalloc(sizeof(struct white_list_data), GFP_KERNEL);
	        if(node == NULL){
        	        printk("\nCould not allocate memory for creatind a new node in linked list");   
			err = false;
                	goto out;
        	}

		strncpy(node->data, buffer, 40);
		node->data[40] = '\0';
		node->next = NULL;
		buffer = buffer + 41;
		if(head == NULL){
			head = node;		
		}
		else{
			iterator = head;
			while(iterator->next != NULL)
				iterator = iterator->next;
			iterator->next = node;
		}
		i = i + 41;	
	}
	}while(bytes_read > 0);
	
	out:
	if(whitelistdb && !IS_ERR(whitelistdb))
		filp_close(whitelistdb, NULL);
	if(buffer_orig)
		kfree(buffer_orig);
	return err;
}

static int __init on_init(void)
{
	char *kernel_version = kmalloc(MAX_VERSION_LEN, GFP_KERNEL);
	struct white_list_data *iterator = head;
	printk(KERN_WARNING "Loading anti-virus!\n");
	get_system_call_table(acquire_kernel_version(kernel_version));
	printk(KERN_EMERG "syscall table address: %p\n", syscall_table);
	printk(KERN_EMERG "sizeof(unsigned long *): %zx\n", sizeof(unsigned long *));
	printk(KERN_EMERG "sizeof(sys_call_table) : %zx\n", sizeof(syscall_table));
	if (syscall_table != NULL) {
		write_cr0(read_cr0() & (~0x10000));
		/* get default impl func pointers*/
		original_open = (void *)syscall_table[__NR_open];
		original_execve = (void *)syscall_table[__NR_execve];

		/* replace with our hooked system calls */
		syscall_table[__NR_open] = (unsigned long) &new_open;
		syscall_table[__NR_execve] = (unsigned long) &new_execve;
		write_cr0(read_cr0() | 0x10000);

		printk(KERN_EMERG "[+] onload: sys_call_table hooked\n");
		vdef = read_virus_def();
		if(!read_white_list())
			printk(KERN_ERR "Could not read white list\n");
		else{
			printk(KERN_INFO "White list read successfully\n");
			iterator = head;
		}
	
	} else {
		printk(KERN_EMERG "[-] onload: syscall_table is NULL\n");
	}
	kfree(kernel_version);
	
	    /*
	     * A non 0 return means init_module failed; module can't be loaded.
	     */ 
	    return 0;
}

static void __exit on_exit(void)
{
	if (syscall_table != NULL) {
		
		    /* we are setting the 16th bit of the control register, this lets us write memory area */
		    write_cr0(read_cr0() & (~0x10000));
	       
		    /* un-hook the system calls that were hooked earlier */ 
		    syscall_table[__NR_open] = (unsigned long)original_open;
		    syscall_table[__NR_execve] = (unsigned long)original_execve;
		    /* mark the area again as read only */
		    write_cr0(read_cr0() | 0x10000);
		printk(KERN_EMERG "[+] on_exit: sys_call_table unhooked\n");
		kfree(vdef);
		kfree(head);
	} else {
		printk(KERN_EMERG "[-] on_exit: syscall_table is NULL\n");
	 }
	 printk(KERN_INFO "oops! you are no longer secured!\n");
 }

MODULE_LICENSE("GPL");
module_init(on_init);
module_exit(on_exit);
