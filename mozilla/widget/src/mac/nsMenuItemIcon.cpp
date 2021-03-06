/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is support for icons in native menu items on Mac OS X.
 *
 * The Initial Developer of the Original Code is Google Inc.
 * Portions created by the Initial Developer are Copyright (C) 2006
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Mark Mentovai <mark@moxienet.com> (Original Author)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * Retrieves and displays icons in native menu items on Mac OS X.
 */


#include "nsMenuItemIcon.h"

#include "prmem.h"
#include "nsIMenu.h"
#include "nsIMenuItem.h"
#include "nsIContent.h"
#include "nsIDocument.h"
#include "nsINameSpaceManager.h"
#include "nsWidgetAtoms.h"
#include "nsIDOMDocumentView.h"
#include "nsIDOMViewCSS.h"
#include "nsIDOMElement.h"
#include "nsIDOMCSSStyleDeclaration.h"
#include "nsIDOMCSSValue.h"
#include "nsIDOMCSSPrimitiveValue.h"
#include "nsIEventQueueService.h"
#include "nsToolkit.h"
#include "nsNetUtil.h"
#include "imgILoader.h"
#include "imgIRequest.h"
#include "gfxIImageFrame.h"
#include "nsIImage.h"
#include "nsIImageMac.h"

#include <Carbon/Carbon.h>


static const PRUint32 kIconWidth = 16;
static const PRUint32 kIconHeight = 16;
static const PRUint32 kIconBitsPerComponent = 8;
static const PRUint32 kIconComponents = 4;
static const PRUint32 kIconBitsPerPixel = kIconBitsPerComponent *
                                          kIconComponents;
static const PRUint32 kIconBytesPerRow = kIconWidth * kIconBitsPerPixel / 8;
static const PRUint32 kIconBytes = kIconBytesPerRow * kIconHeight;


static void
PRAllocCGFree(void* aInfo, const void* aData, size_t aSize) {
  PR_Free((void*)aData);
}


NS_IMPL_ISUPPORTS3(nsMenuItemIcon, imgIContainerObserver, imgIDecoderObserver,
                   imgIDecoderObserver_MOZILLA_1_8_BRANCH)

nsMenuItemIcon::nsMenuItemIcon(nsISupports*                aMenuItem,
                               nsIMenu_MOZILLA_1_8_BRANCH* aMenu,
                               nsIContent*                 aContent)
: mContent(aContent)
, mMenuItem(aMenuItem)
, mMenu(aMenu)
, mMenuRef(NULL)
, mMenuItemIndex(0)
, mLoadedIcon(PR_FALSE)
, mSetIcon(PR_FALSE)
{
}


nsMenuItemIcon::~nsMenuItemIcon()
{
  if (mIconRequest)
    mIconRequest->Cancel(NS_BINDING_ABORTED);
}


nsresult
nsMenuItemIcon::SetupIcon()
{
  nsresult rv;
  if (!mMenuRef || !mMenuItemIndex) {
    // These values are initialized here instead of in the constructor
    // because they depend on the parent menu, mMenu, having inserted
    // this object into its array of children.  That can only happen after
    // the object is constructed.
    rv = mMenu->GetMenuRefAndItemIndexForMenuItem(mMenuItem,
                                                  (void**)&mMenuRef,
                                                  &mMenuItemIndex);
    if (NS_FAILED(rv)) return rv;
  }

  nsCOMPtr<nsIURI> iconURI;
  rv = GetIconURI(getter_AddRefs(iconURI));
  if (NS_FAILED(rv)) {
    // There is no icon for this menu item.  An icon might have been set
    // earlier.  Clear it.
    OSStatus err;
    err = ::SetMenuItemIconHandle(mMenuRef, mMenuItemIndex, kMenuNoIcon, NULL);
    if (err != noErr) return NS_ERROR_FAILURE;

    return NS_OK;
  }

  rv = LoadIcon(iconURI);

  return rv;
}


nsresult
nsMenuItemIcon::GetIconURI(nsIURI** aIconURI)
{
  // Mac native menu items support having both a checkmark and an icon
  // simultaneously, but this is unheard of in the cross-platform toolkit,
  // seemingly because the win32 theme is unable to cope with both at once.
  // The downside is that it's possible to get a menu item marked with a
  // native checkmark and a checkmark for an icon.  Head off that possibility
  // by pretending that no icon exists if this is a checkable menu item.
  nsCOMPtr<nsIMenuItem> menuItem = do_QueryInterface(mMenuItem);
  if (menuItem) {
    nsIMenuItem::EMenuItemType menuItemType;
    menuItem->GetMenuItemType(&menuItemType);
    if (menuItemType == nsIMenuItem::eCheckbox ||
        menuItemType == nsIMenuItem::eRadio)
      return NS_ERROR_FAILURE;
  }

  if (!mContent) return NS_ERROR_FAILURE;

  // First, look at the content node's "image" attribute.
  nsAutoString imageURIString;
  nsresult rv = mContent->GetAttr(kNameSpaceID_None, nsWidgetAtoms::image,
                                  imageURIString);

  if (rv != NS_CONTENT_ATTR_HAS_VALUE) {
    // If the content node has no "image" attribute, get the
    // "list-style-image" property from CSS.
    nsCOMPtr<nsIDOMDocumentView> domDocumentView =
     do_QueryInterface(mContent->GetDocument());
    if (!domDocumentView) return NS_ERROR_FAILURE;

    nsCOMPtr<nsIDOMAbstractView> domAbstractView;
    rv = domDocumentView->GetDefaultView(getter_AddRefs(domAbstractView));
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIDOMViewCSS> domViewCSS = do_QueryInterface(domAbstractView);
    if (!domViewCSS) return NS_ERROR_FAILURE;

    nsCOMPtr<nsIDOMElement> domElement = do_QueryInterface(mContent);
    if (!domElement) return NS_ERROR_FAILURE;

    nsCOMPtr<nsIDOMCSSStyleDeclaration> cssStyleDecl;
    nsAutoString empty;
    rv = domViewCSS->GetComputedStyle(domElement, empty,
                                      getter_AddRefs(cssStyleDecl));
    if (NS_FAILED(rv)) return rv;

    NS_NAMED_LITERAL_STRING(listStyleImage, "list-style-image");
    nsCOMPtr<nsIDOMCSSValue> cssValue;
    rv = cssStyleDecl->GetPropertyCSSValue(listStyleImage,
                                           getter_AddRefs(cssValue));
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIDOMCSSPrimitiveValue> primitiveValue =
     do_QueryInterface(cssValue);
    if (!primitiveValue) return NS_ERROR_FAILURE;

    PRUint16 primitiveType;
    rv = primitiveValue->GetPrimitiveType(&primitiveType);
    if (NS_FAILED(rv)) return rv;
    if (primitiveType != nsIDOMCSSPrimitiveValue::CSS_URI)
      return NS_ERROR_FAILURE;

    rv = primitiveValue->GetStringValue(imageURIString);
    if (NS_FAILED(rv)) return rv;
  }

  // If this menu item shouldn't have an icon, the string will be empty,
  // and NS_NewURI will fail.
  nsCOMPtr<nsIURI> iconURI;
  rv = NS_NewURI(getter_AddRefs(iconURI), imageURIString);
  if (NS_FAILED(rv)) return rv;

  *aIconURI = iconURI;
  NS_ADDREF(*aIconURI);
  return NS_OK;
}


nsresult
nsMenuItemIcon::LoadIcon(nsIURI* aIconURI)
{
  if (mIconRequest) {
    // Another icon request is already in flight.  Kill it.
    mIconRequest->Cancel(NS_BINDING_ABORTED);
    mIconRequest = nsnull;
  }

  mLoadedIcon = PR_FALSE;

  if (!mContent) return NS_ERROR_FAILURE;

  nsCOMPtr<nsIDocument> document = mContent->GetOwnerDoc();
  if (!document) return NS_ERROR_FAILURE;

  nsCOMPtr<nsILoadGroup> loadGroup = document->GetDocumentLoadGroup();
  if (!loadGroup) return NS_ERROR_FAILURE;

  nsresult rv = NS_ERROR_FAILURE;
  nsCOMPtr<imgILoader> loader = do_GetService("@mozilla.org/image/loader;1",
                                              &rv);
  if (NS_FAILED(rv)) return rv;

  if (!mSetIcon) {
    // Set a completely transparent 16x16 image as the icon on this menu item
    // as a placeholder.  This keeps the menu item text displayed in the same
    // position that it will be displayed when the real icon is loaded, and
    // prevents it from jumping around or looking misaligned.

    static PRBool sInitializedPlaceholder;
    static CGImageRef sPlaceholderIconImage;
    if (!sInitializedPlaceholder) {
      sInitializedPlaceholder = PR_TRUE;

      PRUint8* bitmap = (PRUint8*)PR_Malloc(kIconBytes);

      CGColorSpaceRef colorSpace = ::CGColorSpaceCreateDeviceRGB();

      CGContextRef bitmapContext;
      bitmapContext = ::CGBitmapContextCreate(bitmap, kIconWidth, kIconHeight,
                                              kIconBitsPerComponent,
                                              kIconBytesPerRow,
                                              colorSpace,
                                              kCGImageAlphaPremultipliedFirst);
      if (!bitmapContext) {
        PR_Free(bitmap);
        ::CGColorSpaceRelease(colorSpace);
        return NS_ERROR_FAILURE;
      }

      CGRect iconRect = ::CGRectMake(0, 0, kIconWidth, kIconHeight);
      ::CGContextClearRect(bitmapContext, iconRect);
      ::CGContextRelease(bitmapContext);

      CGDataProviderRef provider;
      provider = ::CGDataProviderCreateWithData(NULL, bitmap, kIconBytes,
                                              PRAllocCGFree);
      if (!provider) {
        PR_Free(bitmap);
        ::CGColorSpaceRelease(colorSpace);
        return NS_ERROR_FAILURE;
      }

      sPlaceholderIconImage =
       ::CGImageCreate(kIconWidth, kIconHeight, kIconBitsPerComponent,
                       kIconBitsPerPixel, kIconBytesPerRow, colorSpace,
                       kCGImageAlphaPremultipliedFirst, provider, NULL, TRUE,
                       kCGRenderingIntentDefault);
      ::CGColorSpaceRelease(colorSpace);
      ::CGDataProviderRelease(provider);
    }

    if (!sPlaceholderIconImage) return NS_ERROR_FAILURE;

    OSStatus err;
    err = ::SetMenuItemIconHandle(mMenuRef, mMenuItemIndex, kMenuCGImageRefType,
                                  (Handle)sPlaceholderIconImage);
    if (err != noErr) return NS_ERROR_FAILURE;
  }

  rv = loader->LoadImage(aIconURI, nsnull, nsnull, loadGroup, this,
                         nsnull, nsIRequest::LOAD_NORMAL, nsnull,
                         nsnull, getter_AddRefs(mIconRequest));
  if (NS_FAILED(rv)) return rv;

  // The icon will be picked up in OnStopFrame, which may be called after
  // LoadImage returns.  If the load is to be synchronous, ensure that
  // it completes now.

  if (ShouldLoadSync(aIconURI)) {
    // If there are any failures at this point, just return NS_OK and let
    // the image load asynchronously to completion.

    nsCOMPtr<nsIEventQueueService> eventQueueService = 
     do_GetService(NS_EVENTQUEUESERVICE_CONTRACTID, &rv);
    if (NS_FAILED(rv)) return NS_OK;

    nsCOMPtr<nsIEventQueue> eventQueue;
    rv = eventQueueService->GetSpecialEventQueue(
     nsIEventQueueService::CURRENT_THREAD_EVENT_QUEUE,
     getter_AddRefs(eventQueue));
    if (NS_FAILED(rv)) return NS_OK;

    PLEvent* event;
    rv = NS_OK;
    while (!mLoadedIcon && mIconRequest && NS_SUCCEEDED(rv)) {
      rv = eventQueue->WaitForEvent(&event);
      if (NS_SUCCEEDED(rv))
        rv = eventQueue->HandleEvent(event);
    }
  }

  return NS_OK;
}


PRBool
nsMenuItemIcon::ShouldLoadSync(nsIURI* aURI)
{
  // Older menu managers are unable to cope with menu item icons changing
  // while a menu is open in tracking.  On Panther (10.3), the updated icon
  // will not be displayed and highlighting of menu items in the affected
  // menu will be incorrect until menu tracking ends and the menu is
  // reopened.  On Jaguar (10.2), the updated icon will not be displayed
  // until the menu item is selected or deselected.  Tiger (10.4) does
  // not have these problems.
  //
  // Because icons are set in an imgIDecoderObserver notification, it's
  // possible and even likely that some icons will not be set until after the
  // menu is open.  On systems where this is known to cause trouble,
  // LoadIcon is made to set the icon on the menu item synchronously when
  // the source of the icon is local, as determined by the URI scheme.
#if MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_4
  return PR_FALSE;
#else
  static PRBool sNeedsSync;

  static PRBool sInitialized;
  if (!sInitialized) {
    sInitialized = PR_TRUE;
    sNeedsSync = (nsToolkit::OSXVersion() < MAC_OS_X_VERSION_10_4_HEX);
  }

  if (sNeedsSync) {
    PRBool isLocalScheme;
    if (NS_SUCCEEDED(aURI->SchemeIs("chrome", &isLocalScheme)) &&
        isLocalScheme)
      return PR_TRUE;
    if (NS_SUCCEEDED(aURI->SchemeIs("data", &isLocalScheme)) &&
        isLocalScheme)
      return PR_TRUE;
    if (NS_SUCCEEDED(aURI->SchemeIs("moz-anno", &isLocalScheme)) &&
        isLocalScheme)
      return PR_TRUE;
  }

  return PR_FALSE;
#endif
}


// imgIContainerObserver


NS_IMETHODIMP
nsMenuItemIcon::FrameChanged(imgIContainer*  aContainer,
                             gfxIImageFrame* aFrame,
                             nsIntRect*      aDirtyRect)
{
  return NS_OK;
}


// imgIDecoderObserver


NS_IMETHODIMP
nsMenuItemIcon::OnStartRequest(imgIRequest* aRequest)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStartDecode(imgIRequest* aRequest)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStartContainer(imgIRequest*   aRequest,
                                 imgIContainer* aContainer)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStartFrame(imgIRequest* aRequest, gfxIImageFrame* aFrame)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnDataAvailable(imgIRequest*     aRequest,
                                gfxIImageFrame*  aFrame,
                                const nsIntRect* aRect)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStopFrame(imgIRequest*    aRequest,
                            gfxIImageFrame* aFrame)
{
  if (aRequest != mIconRequest) return NS_ERROR_FAILURE;

  // Only support one frame.
  if (mLoadedIcon)
    return NS_OK;

  nsCOMPtr<gfxIImageFrame> frame = aFrame;
  nsCOMPtr<nsIImage> iimage = do_GetInterface(frame);
  if (!iimage) return NS_ERROR_FAILURE;

  nsCOMPtr<nsIImageMac_MOZILLA_1_8_BRANCH> imageMac = do_QueryInterface(iimage);
  if (!imageMac) return NS_ERROR_FAILURE;

  CGImageRef cgImage;
  nsresult rv = imageMac->GetCGImageRef(&cgImage);
  if (NS_FAILED(rv)) return rv;
  ::CGImageRetain(cgImage);

  // The CGImageRef obtained from the nsIImageMac can't be used as-is.
  // It may not be the right size for a menu icon (16x16), and it's
  // flipped upside-down.  Create a new CGImage for the menu item.
  PRUint8* bitmap = (PRUint8*)PR_Malloc(kIconBytes);

  CGColorSpaceRef colorSpace = ::CGColorSpaceCreateDeviceRGB();
  CGImageAlphaInfo alphaInfo = ::CGImageGetAlphaInfo(cgImage);

  CGContextRef bitmapContext;
  bitmapContext = ::CGBitmapContextCreate(bitmap, kIconWidth, kIconHeight,
                                          kIconBitsPerComponent,
                                          kIconBytesPerRow,
                                          colorSpace,
                                          alphaInfo);
  if (!bitmapContext) {
    ::CGImageRelease(cgImage);
    PR_Free(bitmap);
    ::CGColorSpaceRelease(colorSpace);
    return NS_ERROR_FAILURE;
  }

  // The menu manager expects the icon flipped vertically from the way it
  // comes out of nsIImageMac.  Set up a transform to flip it.
  ::CGContextTranslateCTM(bitmapContext, 0, kIconHeight);
  ::CGContextScaleCTM(bitmapContext, 1, -1);

  CGRect iconRect = ::CGRectMake(0, 0, kIconWidth, kIconHeight);
  ::CGContextClearRect(bitmapContext, iconRect);
  ::CGContextDrawImage(bitmapContext, iconRect, cgImage);
  ::CGImageRelease(cgImage);
  ::CGContextRelease(bitmapContext);

  CGDataProviderRef provider;
  provider = ::CGDataProviderCreateWithData(NULL, bitmap, kIconBytes,
                                            PRAllocCGFree);
  if (!provider) {
    PR_Free(bitmap);
    ::CGColorSpaceRelease(colorSpace);
    return NS_ERROR_FAILURE;
  }

  CGImageRef iconImage =
   ::CGImageCreate(kIconWidth, kIconHeight, kIconBitsPerComponent,
                   kIconBitsPerPixel, kIconBytesPerRow, colorSpace, alphaInfo,
                   provider, NULL, TRUE, kCGRenderingIntentDefault);
  ::CGColorSpaceRelease(colorSpace);
  ::CGDataProviderRelease(provider);
  if (!iconImage) return NS_ERROR_FAILURE;

  OSStatus err;
  err = ::SetMenuItemIconHandle(mMenuRef, mMenuItemIndex, kMenuCGImageRefType,
                                (Handle)iconImage);
  ::CGImageRelease(iconImage);
  if (err != noErr) return NS_ERROR_FAILURE;

  mLoadedIcon = PR_TRUE;
  mSetIcon = PR_TRUE;

  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStopContainer(imgIRequest*   aRequest,
                                imgIContainer* aContainer)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStopDecode(imgIRequest*     aRequest,
                             nsresult         status,
                             const PRUnichar* statusArg)
{
  return NS_OK;
}


NS_IMETHODIMP
nsMenuItemIcon::OnStopRequest(imgIRequest* aRequest,
                              PRBool       aIsLastPart)
{
  mIconRequest->Cancel(NS_BINDING_ABORTED);
  mIconRequest = nsnull;
  return NS_OK;
}
