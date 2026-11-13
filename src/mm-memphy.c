/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   if (mp == NULL || offset >= (addr_t)mp->maxsz)
      return -1;
   int numstep = 0;

   mp->cursor = 0;
   while (numstep < offset && numstep < mp->maxsz)
   {
      /* Traverse sequentially */
      mp->cursor = (mp->cursor + 1) % mp->maxsz;
      numstep++;
   }

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || value == NULL || addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg ==1 )
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   *value = (BYTE)mp->storage[mp->cursor];

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   //B1 : Kiem tra hop le
   if (mp == NULL || value == NULL){
      return -1;
   }
   // B2 : kt gia tri dia chi vat ly
   if (addr >= (addr_t)mp->maxsz){
      return -1;
   }
   // B3 : Doc du lieu tu memphy
   pthread_mutex_lock(&mp->lock);
   int ret = 0;

   if (mp->rdmflg) {
      *value = mp->storage[addr];
   } else {
      ret = MEMPHY_seq_read(mp, addr, value);
   }

   pthread_mutex_unlock(&mp->lock);

   return ret;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{

   if (mp == NULL || addr >= (addr_t)mp->maxsz || value < 0)
      return -1;

   if (mp->rdmflg)
      return -1; /* Not compatible mode for sequential read */

   MEMPHY_mv_csr(mp, addr);
   mp->storage[mp->cursor] = value;

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
// Chinh sua de dong bo nhieu luong
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   //B1  : Kiem tra hop le
   if (mp == NULL){
      return -1;
   }
   // B2 : kt gia tri dia chi vat ly 
   if (addr >= (addr_t)mp->maxsz){
      return -1;
   }
   
   int ret = 0;

   // B3 : Ghi du lieu 
   pthread_mutex_lock(&mp->lock);
   // ghi truc tiep
   if (mp->rdmflg)
   { 
       mp->storage[addr] = data;
   }
   else{
   // ghi tuan tu
      ret = MEMPHY_seq_write(mp, addr, data);
   }
   pthread_mutex_unlock(&mp->lock);

   return ret;
}
/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
//Khoi tao cau truc quan ly bo nho vat ly
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   /* This setting come with fixed constant PAGESZ */
   //B1 :  Kt du lieu 
   if(mp==NULL || pagesz <= 0) 
      return -1;

   //B2 :  Chia so trang nho vat ly thanh cac trang nho co kich thuoc pagesz 
   // tinh so frame
   int numfp = mp->maxsz / pagesz;
   struct framephy_struct *newfst, *fst;
   int iter = 0;

   if (numfp <= 0){
         return -1;
   }
   // Khoi tao danh sach cac trang nho vat ly 
   /* Init head of free framephy list */
   fst = malloc(sizeof(struct framephy_struct));
   // Kiem tra xem co du bo nho de cap phat cho framephy hay khong
   if(fst == NULL){
      return -1;
   }
   fst->fpn = iter;
   mp->free_fp_list = fst;
   fst->fp_next = NULL;
   mp->used_fp_list = NULL;

   //B3 :  Khoi tao cac frame phia sau
   /* We have list with first element, fill in the rest num-1 element member*/
   for (iter = 1; iter < numfp; iter++)
   {
      newfst = malloc(sizeof(struct framephy_struct));
      // Kiem tra xem co du bo nho de cap phat cho framephy hay khong
      if (newfst == NULL) {
   struct framephy_struct *current = mp->free_fp_list;

         while (current != NULL) {
            struct framephy_struct *next = current->fp_next;
            free(current);
            current = next;
         }

         mp->free_fp_list = NULL;
         mp->used_fp_list = NULL;

         return -1;
      }
      newfst->fpn = iter;
      newfst->fp_next = NULL;
      fst->fp_next = newfst;
      fst = newfst;
   }

   return 0;
}
// Lay frame dang roi de su dung
// Return 0 if success, -1 if fail
/* 
 *  MEMPHY_get_freefp - get free framephy from memphy
 *  @mp: memphy struct
 *  @retfpn: tra ve frame number
 */
int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   // b1 : kiem tra gia tri 
   if (mp == NULL || retfpn == NULL){
      return -1;
   }
   pthread_mutex_lock(&mp->lock);
   // critical section
   // B2  : lay con tro free_fp_list - check co frame nao dang free hay khong
   struct framephy_struct *fp = mp->free_fp_list;
  
   // B3 : kiem tra mp co dung dinh dang ko - hoac free list ko co frame nao de tra
   if(fp == NULL){
      pthread_mutex_unlock(&mp->lock);
      return -1; // Khong con frame nao de cap phat
   }

   // B3 :  Co frame free de cap - thi gan vao dau use list
   // Lay frame  
   struct framephy_struct *getFrame =  fp;
   mp->free_fp_list= getFrame->fp_next;
   getFrame->fp_next =  mp->used_fp_list;
   mp->used_fp_list = getFrame; 
   /* 
    * TODO manage free frame list
    */
   *retfpn = getFrame->fpn;

   /* 
    * TODO MEMPHY is iteratively used up until its exhausted
    * No garbage collector acting then it not been released
    */
  
   // critical section end
   pthread_mutex_unlock(&mp->lock);
   return 0;
}
/*
 *  MEMPHY_dump - dump memphy content
 *  @mp: memphy struct
 */
int MEMPHY_dump(struct memphy_struct *mp)
{
   pthread_mutex_lock(&mp->lock);
  /*
   * TODO dump memphy contnt mp->storage
   *     for tracing the memory content
   */
   printf("\n--- MEMPHY DUMP ---\n");
   for (int i = 0; i < mp->maxsz; i++) {
      if (mp->storage[i] != 0) {
         printf("Addr 0x%08x: 0x%02x\n", i, mp->storage[i]);
      }
   }
   printf("-------------------\n");
   pthread_mutex_unlock(&mp->lock);
   return 0;
}
/*
   *  MEMPHY_put_freefp - put free framephy back to memphy
   *  @mp: memphy struct
   *  @fpn: frame number to be put back
*/
// Them mot frame vao danh sach cac frame dang free

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   // B1: Kiem tra gia tri dau vao
   if (mp == NULL) {
      return -1;
   }
#ifdef MM64
   int pagesz = PAGING64_PAGESZ;
#else
   int pagesz = PAGING_PAGESZ;
#endif

   // B2: Kiem tra frame number hop le
   if (fpn >= (addr_t)(mp->maxsz / pagesz)) {
      return -1;
   }

   pthread_mutex_lock(&mp->lock);

   // B3: Lay con tro hai danh sach
   struct framephy_struct *fp = mp->free_fp_list;
   struct framephy_struct *used_fp = mp->used_fp_list;

   // TH1: Kiem tra frame da ton tai trong FREE LIST
   struct framephy_struct *current = fp;

   while (current != NULL) {
      if (current->fpn == fpn) {
         pthread_mutex_unlock(&mp->lock);
         return -1; // Frame da duoc free
      }

      current = current->fp_next;
   }

   // TH2: Tim frame trong USED LIST
   current = used_fp;
   struct framephy_struct *prev = NULL;

   while (current != NULL && current->fpn != fpn) {
      prev = current;
      current = current->fp_next;
   }

   if (current == NULL) {
      // Khong tim thay frame trong USED LIST
      // Truong hop nay hoi vo ly neu ma khoi tao roi thi co o 1 trong 2 nhung rao luon cho chac
      pthread_mutex_unlock(&mp->lock);
      return -1;
   }

   // TH3: Chuyen frame tu USED LIST sang FREE LIST

   // Goi bo node khoi USED LIST
   if (prev == NULL) {
      mp->used_fp_list = current->fp_next;
   } else {
      prev->fp_next = current->fp_next;
   }

   // Them node vao dau FREE LIST
   current->fp_next = mp->free_fp_list;
   mp->free_fp_list = current;
   
   pthread_mutex_unlock(&mp->lock);

   return 0;
}

/*
 *  Init MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   mp->storage = (BYTE *)malloc(max_size * sizeof(BYTE));
   if(mp->storage==NULL){
      return -1;
   }
   mp->maxsz = max_size;
   memset(mp->storage, 0, max_size * sizeof(BYTE));

   // Neu cho them truong giu lai cho dong bo nhieu luong
   pthread_mutex_init(&mp->lock, NULL);

   int pagze;
#ifdef MM64
   pagze = PAGING64_PAGESZ;
#else
   pagze = PAGING_PAGESZ;
#endif
   int succsesFlg =  MEMPHY_format(mp, pagze);
   if(succsesFlg==-1){
      // neu ko lam dc coi nhu that bai va hoan tra
      free(mp->storage);
      mp->storage=NULL;
      pthread_mutex_destroy(&mp->lock);
      return -1;
   }
   mp->rdmflg = (randomflg != 0) ? 1 : 0;

   if (!mp->rdmflg) /* Not Ramdom acess device, then it serial device*/
      mp->cursor = 0;

   return 0;
}

int MEMPHY_destroy(struct memphy_struct *mp){
   if(mp==NULL){
      return -1;
   }
   // Giai phong toan bo cac frame da khoi tao 
    while(mp->free_fp_list!=NULL){
      struct framephy_struct* deleteNode = mp->free_fp_list;
      mp->free_fp_list=mp->free_fp_list->fp_next;
      free(deleteNode);
    }
     while(mp->used_fp_list!=NULL){
      struct framephy_struct* deleteNode = mp->used_fp_list;
      mp->used_fp_list=mp->used_fp_list->fp_next;
      free(deleteNode);
    }
   free(mp->storage);
    mp->storage = NULL;

    pthread_mutex_destroy(&mp->lock);   
    return 0;
}
// #endif