// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id: hw_trick.c 1670 2024-03-03 04:39:25Z wesleyjohnson $
//
// Copyright (C) 1998-2016 by DooM Legacy Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
// $Log: hw_trick.c,v $
// Revision 1.10  2002/07/26 15:22:44  hurdler
//
// Revision 1.9  2001/12/26 17:24:47  hurdler
// Update Linux version
//
// Revision 1.8  2001/05/14 19:02:58  metzgermeister
//   * Fixed floor not moving up with player on E3M1
//   * Fixed crash due to oversized string in screen message ... bad bug!
//   * Corrected some typos
//   * fixed sound bug in SDL
//
// Revision 1.7  2001/05/07 15:55:05  hurdler
// temporary "fix" for heretic
//
// Revision 1.6  2001/04/11 21:14:11  metzgermeister
// Revision 1.5  2001/04/10 18:39:39  metzgermeister
// Revision 1.4  2001/04/09 23:26:06  hurdler
//
// Revision 1.3  2001/04/09 20:23:12  metzgermeister
// more conservative trick treatment
//
// Revision 1.2  2001/03/26 19:47:56  metzgermeister
// more tricky things
//
// Revision 1.1  2001/03/25 18:11:24  metzgermeister
//   * SDL sound bug with swapped stereo channels fixed
//   * separate hw_trick.c now for HW_correctSWTrick(.)
//
//
//
// DESCRIPTION:
//      special trick routines to make some SW tricks look OK with
//      HW rendering. This includes:
//      - deepwatereffect (e.g. tnt/map02)
//      - invisible staircase (e.g. eternal/map02)
//      - floating ceilings (e.g. eternal/map03)
//      
//      It is not guaranteed that it looks identical to the SW mode,
//      but it looks in most of the cases far better than having
//      holes in the architecture, HOM, etc.
//      
//      It fixes as well missing textures, which are replaced by either
//      a default texture or the midtexture.
//      
//      words of notice:
//      pseudosectors, as mentioned in this file, are sectors where both
//      sidedefs point to the same sector. This expression is also used
//      for sectors which are enclosed by another sector but have no
//      correct sidedefs at all
//      
//      if a vertex is inside a poly is determined by the angles between
//      this vertex and all angles on the linedefs (imagine walking along
//      a circle always facing a certain point inside/outside the circle;
//      if inside, angle have taken all values [0..\pi), otherwise the
//      range was < \pi/2
//      
//
//-----------------------------------------------------------------------------

#include <math.h>

#include "doomincl.h"
#include "doomstat.h"

#include "hw_glob.h"
#include "r_local.h"
#include "i_system.h"

//
// add a line to a sectors list of lines
//
static void addLineToChain(sector_t *sector, line_t *line)
{
    linechain_t *thisElem, *nextElem;
    
    if( sector == NULL )
        return;
    
    thisElem = NULL;
    nextElem = sector->sectorLines;
    
    while( nextElem ) // walk through chain
    {
        thisElem = nextElem;
        nextElem = thisElem->next;
    }
    
    // add a new element into the chain
    if(thisElem)
    {
        thisElem->next = malloc(sizeof(linechain_t));
        if(thisElem->next)
        {
            thisElem->next->line = line;
            thisElem->next->next = NULL;
        }
        else
        {
            I_Error("Out of memory in addLineToChain(.)\n");
        }
    }
    else // first element in chain
    {
        sector->sectorLines =  malloc(sizeof(linechain_t));
        if(sector->sectorLines)
        {
            sector->sectorLines->line = line;
            sector->sectorLines->next = NULL;
        }
        else
        {
            I_Error("Out of memory in addLineToChain(.)\n");
        }
    }
}

//
// We dont want a memory hole, do we? ;-)
//
static void releaseLineChains(void)
{
    linechain_t *thisElem, *nextElem;
    sector_t *sector;
    int i;
    
    for(i=0; i<numsectors; i++) 
    {
        sector = &sectors[i];
        nextElem = sector->sectorLines;

        while( nextElem )
        {
            thisElem = nextElem;
            nextElem = thisElem->next;
            free(thisElem);
        }

        sector->sectorLines = NULL;
    }
}

//
// Check if a pseudo sector is valid by checking all it's linedefs
//
static boolean isPSectorValid(sector_t *sector)
{
    linechain_t * thisElem, * nextElem;
    
#ifdef SECTOR_FLAGS
    if( ! (sector->flags & SCF_pseudo_sector) ) // check only pseudosectors, others don't care
#else
    if(!sector->pseudoSector) // check only pseudosectors, others don't care
#endif
    {	
#ifdef PARANOIA
        CONS_Printf("Alert! non-pseudosector fed to isPSectorClosed()\n");
#endif
        return false;
    }

    nextElem = sector->sectorLines;

    while( nextElem )
    {
        thisElem = nextElem;
        nextElem = thisElem->next;
        if(thisElem->line->frontsector != thisElem->line->backsector)
            return false;
    }
    return true;
}

//
// angles are always phiMax-phiMin [0...2\pi)
//
static double phiDiff(double phiMin, double phiMax)
{
  double result;

  result = phiMax-phiMin;

  if(result < 0.0)
        result += 2.0*PI;

  return result;
}

//
// sort phi's so that enclosed angle < \pi
//
static void sortPhi(double phi1, double phi2, double *phiMin, double *phiMax)
{
    if(phiDiff(phi1, phi2) < PI)
    {
        *phiMin = phi1;
        *phiMax = phi2;
    }
    else
    {
        *phiMin = phi2;
        *phiMax = phi1;
    }
}

//
// return if angle(phi1, phi2) is bigger than \pi
// if so, the vertex lies inside the poly
//
static boolean biggerThanPi(double phi1, double phi2)
{
    if(phiDiff(phi1, phi2) > PI)
        return true;
    
    return false;
}

#define DELTAPHI (PI/100.0) // some small phi << \pi

//
// calculate bounds for minimum angle
//
void phiBounds(double phi1, double phi2, double *phiMin, double *phiMax)
{
    double phi1Tmp, phi2Tmp;
    double psi1, psi2, psi3, psi4, psi5, psi6, psi7; // for optimization
    
    sortPhi(phi1, phi2, &phi1Tmp, &phi2Tmp);
    phi1 = phi1Tmp;
    phi2 = phi2Tmp;
    
    // check start condition
    if(*phiMin > PI || *phiMax > PI)
    {
        *phiMin = phi1;
        *phiMax = phi2;
        return;
    }
    
    // 6 cases:
    // new angles inbetween phiMin, phiMax -> forget it
    // new angles enclose phiMin -> set phiMin
    // new angles enclose phiMax -> set phiMax
    // new angles completely outside phiMin, phiMax -> leave largest area free
    // new angles close the range completely!
    // new angles enlarges range on both sides
    
    psi1 = phiDiff(*phiMin, phi1);
    psi2 = phiDiff(*phiMin, phi2);
    psi3 = phiDiff(*phiMax, phi1);
    psi4 = phiDiff(*phiMax, phi2);
    psi5 = phiDiff(*phiMin, *phiMax);
    psi6 = 2.0*PI - psi5; // phiDiff(*phiMax, *phiMin);
    psi7 = 2.0*PI - psi2; // phiDiff(phi2, *phiMin);
    
    // case 1 & 5!
    if((psi1 <= psi5) && (psi2 <= psi5))
    {
        if(psi1 <= psi2) // case 1
        {
            return;
        }
        else // case 5
        {
            // create some artificial interval here not to get into numerical trouble
            // in fact we know now the sector is completely enclosed -> base for computational optimization
            *phiMax = 0.0;
            *phiMin = DELTAPHI;
            return;
        }
    }
    
    // case 2
    if((psi1 >= psi5) && (psi2 <= psi5))
    {
        *phiMin = phi1;
        return;
    }
    
    // case 3
    if((psi3 >= psi6) && (psi4 <= psi6))
    {
        *phiMax = phi2;
        return;
    }
    
    // case 4 & 6
#ifdef PARANOIA
    if((psi3 <= psi6) && (psi4 <= psi6)) // FIXME: isn't this case implicitly true anyway??
#endif
    {
        if(psi3 <= psi4) //case 4
        {
            if(psi3 >= psi7)
            {
                *phiMin = phi1;
                return;
            }
            else
            {
                *phiMax = phi2;
                return;
            }
        }
        else // case 6
        {
            *phiMin = phi1;
            *phiMax = phi2;
            return;
        }
    }

    I_OutputMsg("phiMin = %f, phiMax = %f, phi1 = %f, phi2 = %f\n", *phiMin, *phiMax, phi1, phi2);
    I_Error("phiBounds() freaked out\n");
}

//
// Check if a vertex lies inside a sector
// This works for "well-behaved" convex polygons
// If we need it mathematically correct, we need to sort the 
// linedefs first so we have them in a row, then walk along the linedefs,
// but this is a bit overdone
// 
boolean isVertexInside(vertex_t *vertex, sector_t *sector)
{
    double xa, ya, xe, ye;
    linechain_t *chain;
    double phiMin, phiMax;
    double phi1, phi2;
    
    chain = sector->sectorLines;
    phiMin = phiMax = 10.0*PI; // some value > \pi
    
    while(chain)
    {
        // start and end vertex
        xa = (double)chain->line->v1->x - (double)vertex->x;
        ya = (double)chain->line->v1->y - (double)vertex->y;
        xe = (double)chain->line->v2->x - (double)vertex->x;
        ye = (double)chain->line->v2->y - (double)vertex->y;

        // angle phi of connection between the vertices and the x-axis
        phi1 = atan2(ya, xa);
        phi2 = atan2(ye, xe);

        // if we have just started, we can have to create start bounds for phi

        phiBounds(phi1, phi2, &phiMin, &phiMax);
        chain = chain->next;
    }
    
    return biggerThanPi(phiMin, phiMax);
}


#define MAXSTACK 256 // Not more than 256 polys in each other?
//
// generate a list of sectors which enclose the given sector
//
static void generateStacklist(sector_t *thisSector)
{
    int i;
    int stackCnt;
    
    sector_t *locStacklist[MAXSTACK];
    sector_t *checkSector;
    
    stackCnt = 0;
    
    for(i=0; i<numsectors; i++)
    {
        checkSector = &sectors[i];

        if(checkSector == thisSector) // don't check self
            continue;

        // buggy sector?
        if( thisSector->sectorLines == NULL )
            continue;

        // check if an arbitrary vertex of thisSector lies inside the checkSector
        if(isVertexInside(thisSector->sectorLines->line->v1, checkSector))
        {
            // if so, the thisSector lies inside the checkSector
            locStacklist[stackCnt] = checkSector;
            stackCnt++;

            if(MAXSTACK-1 == stackCnt) // beware of the SIGSEGV! and consider terminating NULL!
                break;
        }
    }

    thisSector->stackList = malloc(sizeof(sector_t*) * (stackCnt+1));
    if( thisSector->stackList == NULL )
    {
        I_Error("Out of memory error in generateStacklist()");
    }
    
    locStacklist[stackCnt] = NULL; // terminating NULL
    
    memcpy(thisSector->stackList, locStacklist, sizeof(sector_t*) * (stackCnt+1));
}

//
// Bubble sort the stacklist with rising lineoutlengths
//
static void sortStacklist(sector_t *sector)
{
    sector_t **list;  // NULL terminated array of ptrs
    sector_t *sec1, *sec2;
    boolean finished;
    int i;
    
    list = sector->stackList;
    finished = false;
    
    if( list[0] == NULL )
        return; // nothing to sort
    
    while(!finished)
    {
        i=0;
        finished = true;

        while( list[i+1] )
        {
            sec1 = list[i];
            sec2 = list[i+1];

            if(sec1->lineoutLength > sec2->lineoutLength)
            {
                list[i] = sec2;
                list[i+1] = sec1;
                finished = false;
            }
            i++;
        }
    }
}

//
// length of a line in euclidian sense
//
static double lineLength(line_t *line)
{
    double dx, dy, length;

    dx = (double) line->v1->x - (double) line->v2->x;
    dy = (double) line->v1->y - (double) line->v2->y;
    
    length = sqrt(dx*dx + dy*dy);
    
    return length;
}


//
// length of the sector lineout
//
static double calcLineoutLength(sector_t *sector)
{
    linechain_t *chain;
    double length;
    
    length = 0.0;
    chain = sector->sectorLines; 
    
    while( chain ) // sum up lengths of all lines
    {
        length += lineLength(chain->line);
        chain = chain->next;
    }
    return length;
}

//
// Calculate length of the sectors lineout
//
static void calcLineouts(sector_t *sector)
{
    sector_t *encSector;
    int secCount;

    secCount = 0;
    encSector = *(sector->stackList);
    
    while( encSector )
    {
        if(encSector->lineoutLength < 0.0) // if length has not yet been calculated
        {
            encSector->lineoutLength = calcLineoutLength(encSector);
        }

        secCount++;
        encSector = *((sector->stackList) + secCount);
    }
}

//
// Free Stacklists of all sectors
//
static void freeStacklists(void)
{
    int i;
    
    for(i=0; i<numsectors; i++)
    {
        if(sectors[i].stackList)
        {
            free(sectors[i].stackList);
            sectors[i].stackList = NULL;
        }
    }
}

//
// if more than half of the toptextures are missing
//
static boolean areToptexturesMissing(sector_t *thisSector)
{
    linechain_t *thisElem, *nextElem;
    sector_t *frontSector, *backSector;
    int nomiss;
    side_t *sdl, *sdr;
    
    thisElem = NULL;
    nextElem = thisSector->sectorLines;
    nomiss = 0;
    
    while( nextElem ) // walk through chain
    {
        thisElem = nextElem;
        nextElem = thisElem->next;

        frontSector = thisElem->line->frontsector;
        backSector  = thisElem->line->backsector;
        if( ! backSector || ! frontSector )
           continue;

        if(frontSector == backSector) // skip damn renderer tricks here
        {
            continue;
        }

        sdr = &sides[thisElem->line->sidenum[0]];
        sdl = &sides[thisElem->line->sidenum[1]];

        if(backSector->ceilingheight < frontSector->ceilingheight)
        {
            // texture num are either 0=no-texture, or valid
            if(sdr->toptexture != 0)
            {
                nomiss++;
                break; // we can stop here if decision criterium is ==0
            }
        }

        else if(backSector->ceilingheight > frontSector->ceilingheight)
        {
            // texture num are either 0=no-texture, or valid
            if(sdl->toptexture != 0)
            {
                nomiss++;
                break; // we can stop here if decision criterium is ==0
            }
        }
    }
    
    return nomiss == 0;
}

//
// are more textures missing than present?
//
static boolean areBottomtexturesMissing(sector_t *thisSector)
{
    linechain_t *thisElem, *nextElem;
    sector_t *frontSector, *backSector;
    int nomiss;
    side_t *sdl, *sdr;
    
    thisElem = NULL;
    nextElem = thisSector->sectorLines;
    nomiss = 0;
    
    while( nextElem ) // walk through chain
    {
        thisElem = nextElem;
        nextElem = thisElem->next;

        frontSector = thisElem->line->frontsector;
        backSector  = thisElem->line->backsector;
        if( ! backSector || ! frontSector )
           continue;

        if(frontSector == backSector) // skip damn renderer tricks here
        {
           continue;
        }

        sdr = &sides[thisElem->line->sidenum[0]];
        sdl = &sides[thisElem->line->sidenum[1]];

        if(backSector->floorheight > frontSector->floorheight)
        {
            // texture num are either 0=no-texture, or valid
            if(sdr->bottomtexture != 0)
            {
                nomiss++;
                break; // we can stop here if decision criterium is ==0
            }
        }

        else if(backSector->floorheight < frontSector->floorheight)
        {
            // texture num are either 0=no-texture, or valid
            if(sdl->bottomtexture != 0)
            {
                nomiss++;
                break; // we can stop here if decision criterium is ==0
            }
        }
    }
    
    //    return missing >= nomiss;
    return nomiss == 0;
}

//
// check if no adjacent sector has same ceiling height
//
static boolean isCeilingFloating(sector_t *thisSector)
{
    sector_t *adjSector, *refSector, *frontSector, *backSector;
    boolean floating = true;
    linechain_t *thisElem, *nextElem;
    
    if( thisSector == NULL )
        return false;

    refSector = NULL;
    thisElem  = NULL;
    nextElem  = thisSector->sectorLines;
    
    while( nextElem ) // walk through chain
    {
        thisElem = nextElem;
        nextElem = thisElem->next;

        frontSector = thisElem->line->frontsector;
        backSector  = thisElem->line->backsector;

        if(frontSector == thisSector)
            adjSector = backSector;
        else
            adjSector = frontSector;

        if( adjSector == NULL ) // assume floating sectors have surrounding sectors
        {
            floating = false;
            break;
        }

        if( refSector == NULL )
        {
            refSector = adjSector;
            continue;
        }

        // if adjacent sector has same height or more than one adjacent sector exists -> stop
        if(thisSector->ceilingheight == adjSector->ceilingheight ||
           refSector != adjSector)
        {
            floating = false;
            break;
        }
    }
    
    // now check for walltextures
    if(floating)
    {
        if(!areToptexturesMissing(thisSector))
        {
            floating = false;
        }
    }
    return floating;
}

//
// check if no adjacent sector has same ceiling height
// FIXME: throw that together with isCeilingFloating??
//
static boolean isFloorFloating(sector_t *thisSector)
{
    sector_t *adjSector, *refSector, *frontSector, *backSector;
    boolean floating = true;
    linechain_t *thisElem, *nextElem;
    
    if( thisSector == NULL )
        return false;

    refSector = NULL;
    thisElem  = NULL;
    nextElem  = thisSector->sectorLines;
    
    while( nextElem ) // walk through chain
    {
        thisElem = nextElem;
        nextElem = thisElem->next;

        frontSector = thisElem->line->frontsector;
        backSector  = thisElem->line->backsector;

        if(frontSector == thisSector)
            adjSector = backSector;
        else
            adjSector = frontSector;

        if( adjSector == NULL ) // assume floating sectors have surrounding sectors
        {
            floating = false;
            break;
        }

        if( refSector == NULL )
        {
            refSector = adjSector;
            continue;
        }

        // if adjacent sector has same height or more than one adjacent sector exists -> stop
        if(thisSector->floorheight == adjSector->floorheight ||
           refSector != adjSector)
        {
            floating = false;
            break;
        }
    }

    // now check for walltextures
    if(floating)
    {
        if(!areBottomtexturesMissing(thisSector))
        {
            floating = false;
        }
    }
    return floating;
}

//
// estimate ceilingheight according to height of adjacent sector
//
static fixed_t estimateCeilHeight(sector_t *thisSector)
{
    sector_t *adjSector;

    if( (thisSector == NULL)
        || (thisSector->sectorLines == NULL)
        || (thisSector->sectorLines->line == NULL) )
        return 0;
    
    adjSector = thisSector->sectorLines->line->frontsector;
    if(adjSector == thisSector)
        adjSector = thisSector->sectorLines->line->backsector;
    
    if( adjSector == NULL )
        return 0;
    
    return adjSector->ceilingheight;
}

//
// estimate ceilingheight according to height of adjacent sector
//
static fixed_t estimateFloorHeight(sector_t *thisSector)
{
    sector_t *adjSector;
    
    if( (thisSector == NULL)
        || (thisSector->sectorLines == NULL)
        || (thisSector->sectorLines->line == NULL) )
        return 0;
    
    adjSector = thisSector->sectorLines->line->frontsector;
    if(adjSector == thisSector)
        adjSector = thisSector->sectorLines->line->backsector;
    
    if( adjSector == NULL )
        return 0;
    
    return adjSector->floorheight;
}

#define CORRECT_FLOAT_EXPERIMENTAL


// --------------------------------------------------------------------------
// Some levels have missing sidedefs, which produces HOM, so let's try to
// compensate for that
// and some levels have deep water trick, invisible staircases etc.
// --------------------------------------------------------------------------
// FIXME: put some nice default texture in legacy.dat and use it
// Called from P_SetupLevel, HWR_SetupLevel.
void HWR_CorrectSWTricks(void)
{
    int i, k;
    line_t *ld;
    side_t *sdl = NULL, *sdr;
    sector_t *secl, *secr;
    sector_t **sectorList;
    sector_t *outSector;
    
    if ( EN_heretic_hexen || (cv_grcorrecttricks.value == 0) )
        return;
    
    // determine lines for sectors
    for(i=0; i<numlines; i++)
    {
        ld = &lines[i];
        secr = ld->frontsector;
        secl = ld->backsector;

        if(secr == secl)
        {
#ifdef SECTOR_FLAGS
            secr->flags |= SCF_pseudo_sector; // special renderer trick?
#else
            secr->pseudoSector = true; // special renderer trick?
#endif
            addLineToChain(secr, ld);
        }
        else
        {
            addLineToChain(secr, ld);
            addLineToChain(secl, ld);
        }
    }
    
    // preprocessing
    for(i=0; i<numsectors; i++)
    {
        sector_t *checkSector;

        checkSector = &sectors[i];

        // identify real pseudosectors first
#ifdef SECTOR_FLAGS
        if( checkSector->flags & SCF_pseudo_sector )
#else
        if(checkSector->pseudoSector)
#endif
        {
            if(!isPSectorValid(checkSector)) // drop invalid pseudo sectors
            {
#ifdef SECTOR_FLAGS
                checkSector->flags &= ~(SCF_pseudo_sector);  // clear pseudo sector flag
#else
                checkSector->pseudoSector = false;
#endif
            }
        }

        // determine enclosing sectors for pseudosectors ... used later
#ifdef SECTOR_FLAGS
        if( checkSector->flags & SCF_pseudo_sector )
#else
        if(checkSector->pseudoSector)
#endif
        {
            generateStacklist(checkSector);
            calcLineouts(checkSector);
            sortStacklist(checkSector);
        }
    }
    
    // set virtual floor heights for pseudo sectors
    // required for deep water effect e.g.
    for(i=0; i<numsectors; i++)
    {
#ifdef SECTOR_FLAGS
        if( sectors[i].flags & SCF_pseudo_sector )
#else	
        if(sectors[i].pseudoSector)
#endif
        {
            sectorList = sectors[i].stackList;
            k = 0;
            while( sectorList[k] )
            {
                outSector = sectorList[k];
#ifdef SECTOR_FLAGS
                if( ! (outSector->flags & SCF_pseudo_sector) )
#else
                if(!outSector->pseudoSector)
#endif
                {
                    sectors[i].virtualFloorheight = outSector->floorheight;
                    sectors[i].virtualCeilingheight = outSector->ceilingheight;
                    break;
                }
                k++;
            }
            if(sectorList[k] == NULL) // sorry, did not work :(
            {
                sectors[i].virtualFloorheight = sectors[i].floorheight;
                sectors[i].virtualCeilingheight = sectors[i].ceilingheight;
            }
        }
    }
#ifdef CORRECT_FLOAT_EXPERIMENTAL
    // correct ceiling/floor heights of totally floating sectors
    for(i=0; i<numsectors; i++)
    {
        sector_t *floatSector;

        floatSector = &sectors[i];

        // correct height of floating sectors
        if(isCeilingFloating(floatSector))
        {
            fixed_t corrheight;

            corrheight = estimateCeilHeight(floatSector);
            floatSector->virtualCeilingheight = corrheight;
#ifdef SECTOR_FLAGS
            floatSector->flags |= SCF_virtual_ceiling;
#else
            floatSector->virtualCeiling = true;
#endif
        }
        if(isFloorFloating(floatSector))
        {
            fixed_t corrheight;

            corrheight = estimateFloorHeight(floatSector);
            floatSector->virtualFloorheight = corrheight;
#ifdef SECTOR_FLAGS
            floatSector->flags |= SCF_virtual_floor;
#else
            floatSector->virtualFloor = true;
#endif
        }
    }
#endif

    // now for the missing textures
    for(i=0; i<numlines; i++)
    {
        ld = &lines[i];
        sdr = &sides[ld->sidenum[0]];
        if (ld->sidenum[1] != NULL_INDEX)
        {
            sdl = &sides[ld->sidenum[1]];
        }

        secr = ld->frontsector;
        secl = ld->backsector;

        if(secr == secl) // special renderer trick
            continue; // we can't correct missing textures here

        if( secl ) // only if there is a backsector
        {
#ifdef SECTOR_FLAGS
            // If secr or secl are pseudo sector.
            if( (secr->flags | secl->flags) & SCF_pseudo_sector )
#else
            if(secr->pseudoSector || secl->pseudoSector)
#endif
                continue;

#ifdef SECTOR_FLAGS
            // If both secr and secl are not virtual_floor.
            if( !( (secr->flags | secl->flags) & SCF_virtual_floor ) )
#else
            if(!secr->virtualFloor && !secl->virtualFloor)
#endif
            {
                if(secl->floorheight > secr->floorheight)
                {
                    // now check if r-sidedef is correct
                    // texture num are either 0=no-texture, or valid
                    if(sdr->bottomtexture == 0)
                    {
                        if(sdr->midtexture == 0)
                            sdr->bottomtexture = R_TextureNumForName("STONE2");
                        else
                            sdr->bottomtexture = sdr->midtexture;
                    }
                }
                else if(secl->floorheight < secr->floorheight)
                {
                    // now check if l-sidedef is correct
                    // texture num are either 0=no-texture, or valid
                    if(sdl->bottomtexture == 0)
                    {
                        if(sdl->midtexture == 0)
                            sdl->bottomtexture = R_TextureNumForName("STONE2");
                        else
                            sdl->bottomtexture = sdl->midtexture;
                    }
                }
            }

#ifdef SECTOR_FLAGS
            // If both secr and secl are not virtual_ceiling.
            if( !( (secr->flags | secl->flags) & SCF_virtual_ceiling ) )
#else
            if(!secr->virtualCeiling && !secl->virtualCeiling)
#endif
            {
                if(secl->ceilingheight < secr->ceilingheight)
                {
                    // now check if r-sidedef is correct
                    // texture num are either 0=no-texture, or valid
                    if(sdr->toptexture == 0)
                    {
                        if(sdr->midtexture == 0)
                            sdr->toptexture = R_TextureNumForName("STONE2");
                        else
                            sdr->toptexture = sdr->midtexture;
                    }
                }
                else if(secl->ceilingheight > secr->ceilingheight)
                {
                    // now check if l-sidedef is correct
                    if(sdl->toptexture == 0)
                    {
                        if(sdl->midtexture == 0)
                            sdl->toptexture = R_TextureNumForName("STONE2");
                        else
                            sdl->toptexture = sdl->midtexture;

                    }
                }
            }
        } // if(NULL != secl)
    } // for(i=0; i<numlines; i++)

    // release all linechains
    releaseLineChains();
    freeStacklists();
}
