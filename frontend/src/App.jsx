
import { useEffect, useState } from 'react'
import './App.css'

function App() {
  const [images, setImages] = useState([])
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState(null)
  const [selectedImage, setSelectedImage] = useState(null)
  const [battery, setBattery] = useState(null)
  const [batteryError, setBatteryError] = useState(null)

  const [viewMode, setViewMode] = useState('list')
  const [pageSize, setPageSize] = useState(10)
  const [currentPage, setCurrentPage] = useState(1)

  const selectedImageIndex = images.findIndex(
    (image) => image.id === selectedImage?.id
  )

  const previousImage = () => {
    if (selectedImageIndex > 0) {
      setSelectedImage(images[selectedImageIndex - 1])
    }
  }

  const nextImage = () => {
    if (selectedImageIndex < images.length - 1) {
      setSelectedImage(images[selectedImageIndex + 1])
    }
  }
  useEffect(() => {
  const handleKeyDown = (event) => {
    if (!selectedImage) return

    if (event.key === 'Escape') {
      setSelectedImage(null)
    }

    if (event.key === 'ArrowLeft') {
      previousImage()
    }

    if (event.key === 'ArrowRight') {
      nextImage()
    }
  }

  window.addEventListener('keydown', handleKeyDown)

  return () => {
    window.removeEventListener('keydown', handleKeyDown)
  }
}, [selectedImage, selectedImageIndex])
  useEffect(() => {
    async function fetchImages() {
      try {
        const response = await fetch('/api/images')

        if (!response.ok) {
          throw new Error('Nie udało się pobrać zdjęć')
        }

        const data = await response.json()
        setImages(data.images)

      } catch (error) {
        console.error('Image loading failed:', error)
        setError(error.message)

      } finally {
        setLoading(false)
      }
    }

    fetchImages()
  }, [])

  useEffect(() => {
    async function fetchBattery() {
      try {
        const response = await fetch('/api/telemetry')

        if (!response.ok) {
          throw new Error('Nie udało się pobrać stanu baterii')
        }

        const data = await response.json()
        setBattery(data.battery)
        setBatteryError(null)

      } catch (error) {
        console.error('Battery loading failed:', error)
        setBatteryError(error.message)
      }
    }

    fetchBattery()

  }, [])

  const totalPages = Math.ceil(images.length / pageSize)

  const startIndex = (currentPage - 1) * pageSize
  const endIndex = Math.min(startIndex + pageSize, images.length)

  const visibleImages = images.slice(startIndex, endIndex)

  if (loading) {
    return <p>Ładowanie zdjęć...</p>
  }

  if (error) {
    return <p>Błąd: {error}</p>
  }

  return (
    <main>
    <div className="dashboard-header">
      <h1>System kontroli bezpieczeństwa</h1>
        <section className="dashboard-status">
          <div className="battery-card">
            <div className="battery-header">
              <span>Stan baterii</span>
              <span className="battery-icon">🔋</span>
            </div>

            {batteryError ? (
              <p className="battery-error">Błąd pobierania baterii</p>
            ) : battery === null ? (
              <p>Brak danych</p>
            ) : (
              <>
                <div className="battery-value">{battery}%</div>

                <div className="battery-bar">
                  <div
                    className={`battery-fill ${
                      battery <= 20
                        ? 'low'
                        : battery <= 59
                          ? 'medium'
                          : 'high'
                    }`}
                    style={{ width: `${battery}%` }}
                  />
                </div>
              </>
            )}
          </div>
        </section>
      </div>


      <div className="gallery-toolbar">
        <div className="view-controls">
          <button
            type="button"
            className={viewMode === 'list' ? 'active' : ''}
            onClick={() => setViewMode('list')}
          >
            Lista
          </button>

          <button
            type="button"
            className={viewMode === 'grid' ? 'active' : ''}
            onClick={() => setViewMode('grid')}
          >
            Siatka
          </button>
        </div>

        <div className="page-size-control">
          <label htmlFor="pageSize">Zdjęć na stronie:</label>

          <select
            id="pageSize"
            value={pageSize}
            onChange={(event) => {
              setPageSize(Number(event.target.value))
              setCurrentPage(1)
            }}
          >
            <option value={5}>5</option>
            <option value={10}>10</option>
            <option value={20}>20</option>
            <option value={50}>50</option>
          </select>
        </div>
      </div>

      <p>
        Wyświetlono {images.length === 0 ? 0 : startIndex + 1}
        {'–'}
        {endIndex} z {images.length} zdjęć
      </p>

      {images.length === 0 ? (
        <p>Brak zapisanych zdjęć.</p>
      ) : (
        <div className={`gallery ${viewMode}`}>
          {visibleImages.map((image) => (
            <div className="gallery-item" key={image.id}>
              <img
                src={`/api/images/${image.id}/file`}
                alt={`Zdjęcie ${image.id}`}
                onClick={() => setSelectedImage(image)}
              />
              <div>
                <h3>
                  {new Date(image.event_timestamp).toLocaleString('pl-PL')}
                </h3>
                <p>Zdjęcie #{image.id}</p>
              </div>
            </div>
          ))}
        </div>
      )}

      {selectedImage && (
        <div
          className="image-modal"
          onClick={() => setSelectedImage(null)}
        >
          <div
            className="image-modal-content"
            onClick={(event) => event.stopPropagation()}
          >
            <button
              type="button"
              className="image-modal-close"
              onClick={() => setSelectedImage(null)}
            >
              &times;
            </button>

            <button
              type="button"
              className="image-modal-nav previous"
              onClick={previousImage}
              disabled={selectedImageIndex <= 0}
            >
              &#10094;
            </button>

            <button
              type="button"
              className="image-modal-nav next"
              onClick={nextImage}
              disabled={selectedImageIndex >= images.length - 1}
            >
              &#10095;
            </button>
            <img
              src={`/api/images/${selectedImage.id}/file`}
              alt={`Zdjęcie ${selectedImage.id}`}
            />

            <div className="image-modal-info">
              <h2>
                {new Date(selectedImage.event_timestamp).toLocaleString('pl-PL')}
              </h2>
              <p>Zdjęcie #{selectedImage.id}</p>
            </div>
          </div>
        </div>
      )}

      <div className="pagination">
        <button
          type="button"
          disabled={currentPage === 1}
          onClick={() => setCurrentPage((page) => page - 1)}
        >
          Poprzednia
        </button>

        <span>
          Strona {currentPage} z {Math.max(totalPages, 1)}
        </span>

        <button
          type="button"
          disabled={currentPage >= totalPages}
          onClick={() => setCurrentPage((page) => page + 1)}
        >
          Następna
        </button>
      </div>
    </main>
  )
}

export default App
