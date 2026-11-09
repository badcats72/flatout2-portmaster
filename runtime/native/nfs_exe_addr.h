/* FlatOut 2 (Zoom Platform FlatOut2.exe, 3,004,832 bytes, built 2006-09-29).
 *
 * The NFSU2 runtime this comes from names SPEED2.EXE code by address (NFS_VA(0x...), sub_XXXXXXXX
 * wrappers). NFS_VA() gives an address no guest call or return ever has, so every NFSU2-specific branch
 * stays off. */
#ifndef NFS_EXE_ADDR_H
#define NFS_EXE_ADDR_H

#define NFS_VA(x) 0xFFFFFFF0u

#define NFS_EXE_ID "fo2-zoom"
#define NFS_EXE_NAME "FlatOut2.exe"
#define NFS_EXE_SIZE 3004832u
#define NFS_EXE_BUILT_FOR "FO2_EXE_BUILT_FOR=fo2-zoom:3004832"   /* read by the launcher */

#endif
