import numpy as np
import os

from datetime          import datetime
from time              import perf_counter
from astropy.io        import fits
from scipy.interpolate import griddata


# Physical constants
c     = 2.99792458E+08   # [m/s] speed of light
h     = 6.62607004E-34   # [J*s] Planck's constant
kb    = 1.38064852E-23   # [J/K] Boltzmann's constant
amu   = 1.66053904E-27   # [kg] atomic mass unit
T_CMB = 2.72548000E+00   # [K] CMB temperature


def timestamp():
    """
    Returns a time stamp for the current date and time.
    """
    return datetime.now().strftime('%y%m%d-%H%M%S')


class Timer ():
    def __init__ (self, name):
        self.name      = name
        self.starts    = []
        self.stops     = []
        self.intervals = []
        self.total     = 0.0
    def start (self):
        self.starts.append(perf_counter())
    def stop (self):
        self.stops.append(perf_counter())
        self.intervals.append(self.stops[-1]-self.starts[-1])
        self.total += self.intervals[-1]
    def print (self):
        return f'timer: {self.name} = {self.total}'


def relative_error (a,b):
    """
    Returns the relative error between a and b.
    """
    return 2.0*(a-b)/(a+b)


def LTEpop (linedata, temperature):
    '''
    Return the LTE level populations give the temperature.
    '''
    pop = np.zeros(linedata.nlev)
    # Calculate the LTE populations
    for i in range(linedata.nlev):
        pop[i] = linedata.weight[i] * np.exp(-linedata.energy[i]/(kb*temperature))
    # Normalize to relative populations
    pop = pop / sum(pop)
    # Done
    return pop


def lineEmissivity (linedata, pop):
    '''
    Return the line emissivity for each radiative transition.
    '''
    eta = np.zeros(linedata.nrad)
    for k in range(linedata.nrad):
        i = linedata.irad[k]
        j = linedata.jrad[k]
        eta[k] = h*linedata.frequency[k]/(4.0*np.pi) * linedata.A[k]*pop[i]
    # Done
    return eta


def lineOpacity (linedata, pop):
    '''
    Return the line opacity for each radiative transition.
    '''
    chi = np.zeros(linedata.nrad)
    for k in range(linedata.nrad):
        i = linedata.irad[k]
        j = linedata.jrad[k]
        chi[k] = h*linedata.frequency[k]/(4.0*np.pi) * (linedata.Ba[k]*pop[j] - linedata.Bs[k]*pop[i])
    # Done
    return chi


def lineSource (linedata, pop):
    '''
    Return the line source function for each radiative transition.
    '''
    S = lineEmissivity (linedata, pop) / lineOpacity (linedata, pop)
    # Done
    return S


def planck (temperature, frequency):
    '''
    Planck function for thermal radiation.
    '''
    return 2.0*h/c**2 * np.power(frequency,3) / np.expm1(h*frequency/(kb*temperature))


def I_CMB (frequency):
    """
    Intensity of the cosmic microwave background.
    """
    return planck (T_CMB, frequency)


def dnu (linedata, k, temp, vturb2):
    """
    :param linedata: Magritte linedata object
    :param k: transition number
    :param temp: temperature
    :param turb: turbulent velocity
    :return: line width of the line profile function.
    """
    return linedata.frequency[k] * np.sqrt(2.0*kb*temp/(amu*c**2)*linedata.inverse_mass + vturb2)


def profile (linedata, k, temp, vturb2, nu):
    """
    :param linedata: Magritte linedata object
    :param k: transition number
    :param temp: temperature
    :param turb: turbulent velocity
    :param nu: frequency at which to evaluate
    :return: Gaussian profile function evaluated at frequency nu.
    """
    x = (nu - linedata.frequency[k]) / dnu(linedata, k, temp, vturb2)
    return np.exp(-x**2) / (np.sqrt(np.pi) * dnu(linedata, k, temp, vturb2))


def save_fits(
        model,
        filename   = None,
        image_nr   =  -1,
        zoom       = 1.3,
        npix_x     = 300,
        npix_y     = 300,
        method     = 'nearest',
    ):
    """
    Save channel maps of synthetic observation (image) as fits file.
    """
    
    # Check if there are images
    if (len(model.images) < 1):
        print('No images in model.')
        return
    
    if not filename:
        # Get path of image directory
        im_dir = os.path.dirname(os.path.abspath(model.parameters.model_name())) + '/images/'
        # If no image directory exists yet
        if not os.path.exists(im_dir):
            # Create image directory
            os.makedirs(im_dir)
            print('Created image directory:', im_dir)
        # Define filename
        filename = f"{im_dir}image.fits"
        
    # Remove fits file if it already exists
    if os.path.isfile(filename):
        os.remove(filename)
    
    # Extract image data
    imx = np.array(model.images[image_nr].ImX)
    imy = np.array(model.images[image_nr].ImY)
    imI = np.array(model.images[image_nr].I)

    # Extract the number of frequency bins
    nfreqs = model.parameters.nfreqs()
    
    # Set image boundaries
    x_min, x_max = np.min(imx)/zoom, np.max(imx)/zoom
    y_min, y_max = np.min(imy)/zoom, np.max(imy)/zoom

    # Create image grid values
    xs = np.linspace(x_min, x_max, npix_x)
    ys = np.linspace(y_min, y_max, npix_y)
    
    # Extract the spectral / velocity data
    freqs = np.array(model.radiation.frequencies.nu)[0]
    f_ij  = np.mean(freqs)

    dpix_x = np.mean(np.diff(xs))
    dpix_y = np.mean(np.diff(ys))
    dfreqs = np.diff(freqs)
    
    if (np.abs(relative_error(np.max(dfreqs), np.min(dfreqs))) > 1.0e-9):
        print('WARNING: No regularly spaced frequency bins!')
        dfreq = None
    else:
        dfreq = np.mean(dfreqs)
    
    # Interpolate the scattered data to an image (regular grid)
    zs = np.zeros((nfreqs, npix_x, npix_y))
    for f in range(nfreqs):
        # Nearest neighbor interpolate scattered image data
        zs[f] = griddata((imx, imy), imI[:,f], (xs[None,:], ys[:,None]), method=method)
    
    hdr = fits.Header()
    hdr['NAXIS'   ] = 3        # dims of the cube
    hdr['NAXIS1'  ] = nfreqs   # number of pixels along velocity-axis
    hdr['NAXIS2'  ] = npix_x   # number of pixels along x axis (hor)
    hdr['NAXIS3'  ] = npix_y   # number of pixels along y-axis (ver)
    hdr['CENTFREQ'] = f_ij     # central frequency of the image [Hz]
    hdr['DPIX_X'  ] = dpix_x   # pixel size in meter [m] along x-axis
    hdr['DPIX_Y'  ] = dpix_y   # pixel size in meter [m] along y-axis
    hdr['DFREQ'   ] = dfreq    # pixel size in Hertz [Hz] along frequency axis
    hdr['IM_UNIT' ] = 'W m^-2 Hz^-1 ster^-1'
    
    fits.writeto(filename, data=zs, header=hdr)
    
    print('Written file to:', filename)